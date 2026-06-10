package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math"
	"sort"
	"sync"
	"time"
)

// QueueTask is the recipe for a deferred send. We store the *intent*
// (recipient(s), amount, send_all flag, fee_rate override, manual
// inputs, schedule), NOT the signed bytes. At execution time the
// runner re-builds + re-signs against the current wallet state — fee
// and change are recalculated, UTXOs are picked from whatever the
// wallet has on chain at that moment. This means a queued task is
// resilient to the wallet having received new UTXOs since it was
// queued, and it gracefully fails if the funds have moved.
//
// Lifecycle:
//   pending   created, awaiting either /run or scheduled_at firing
//   running   executor picked it up; brief, holds across one build+broadcast
//   done      broadcast accepted by node; tx_id populated
//   failed    build or broadcast rejected; error populated
//
// State terminology mirrors the QueueTask.Status JSON field below.
const (
	QueueStatusPending = "pending"
	QueueStatusRunning = "running"
	QueueStatusDone    = "done"
	QueueStatusFailed  = "failed"
)

// QueueTask is the persisted (in-memory) record of a queued send.
type QueueTask struct {
	ID     string `json:"id"`
	Wallet string `json:"wallet"`

	// Recipe — the same shape /api/wallet/send accepts.
	To        string         `json:"to,omitempty"`
	AmountSat int64          `json:"amount_sat,omitempty"`
	Outputs   []QueueOutput  `json:"outputs,omitempty"`
	SendAll   bool           `json:"send_all,omitempty"`
	FeeRate   int64          `json:"fee_rate_sat_per_vb,omitempty"`
	Inputs    []QueueInput   `json:"inputs,omitempty"`
	// ChangeAddress (P2.3) — canonical change destination override.
	ChangeAddress string     `json:"change_address,omitempty"`

	// Schedule — nil = manual broadcast only.
	ScheduledAt *time.Time `json:"scheduled_at,omitempty"`

	// State — managed by the runner.
	Status     string     `json:"status"`
	TxID       string     `json:"tx_id,omitempty"`
	Error      string     `json:"error,omitempty"`
	CreatedAt  time.Time  `json:"created_at"`
	ExecutedAt *time.Time `json:"executed_at,omitempty"`
}

type QueueOutput struct {
	Address   string `json:"address"`
	AmountSat int64  `json:"amount_sat"`
	// Max (P2.4) — this output absorbs the remainder (amount computed).
	Max bool `json:"max,omitempty"`
}

type QueueInput struct {
	TxID       string `json:"txid,omitempty"`
	Vout       uint32 `json:"vout,omitempty"`
	Commitment string `json:"commitment,omitempty"`
}

// QueueRegistry is the in-memory store + scheduler ticker.
//
// Like WalletRegistry the queue is NOT persisted; a process restart
// drops everything. This matches the wallet model (handles in RAM
// only) and avoids edge cases around stale schedule firing after
// a restart with new UTXO state.
type QueueRegistry struct {
	mu       sync.Mutex
	tasks    map[string]*QueueTask
	executor TaskExecutor
	ticker   *time.Ticker
	stop     chan struct{}
}

// TaskExecutor is the dependency the runner pulls in to actually
// build+sign+broadcast a recipe. Decoupled from API so queue.go has
// no dependency on http handlers / cgo wallet ops directly.
type TaskExecutor interface {
	ExecuteRecipe(task *QueueTask) (txid string, err error)
}

func newQueueRegistry(exec TaskExecutor) *QueueRegistry {
	q := &QueueRegistry{
		tasks:    make(map[string]*QueueTask),
		executor: exec,
		stop:     make(chan struct{}),
	}
	q.ticker = time.NewTicker(5 * time.Second)
	go q.runLoop()
	return q
}

// Stop signals the scheduler goroutine to exit. Used in tests + on
// graceful shutdown.
func (q *QueueRegistry) Stop() {
	q.ticker.Stop()
	close(q.stop)
}

// runLoop polls every tick for pending tasks whose scheduled_at has
// passed, and runs them sequentially. Sequential is intentional:
// concurrent execution would race on the same wallet's UTXO set and
// produce inconsistent error patterns.
func (q *QueueRegistry) runLoop() {
	for {
		select {
		case <-q.stop:
			return
		case <-q.ticker.C:
			q.runDueTasks()
		}
	}
}

func (q *QueueRegistry) runDueTasks() {
	now := time.Now()
	q.mu.Lock()
	due := make([]*QueueTask, 0)
	for _, t := range q.tasks {
		if t.Status != QueueStatusPending {
			continue
		}
		if t.ScheduledAt == nil {
			continue // manual-only
		}
		if t.ScheduledAt.After(now) {
			continue // not yet
		}
		due = append(due, t)
	}
	// Run oldest-scheduled first so timestamp ordering is honored when
	// many tasks fire on the same tick.
	sort.Slice(due, func(i, j int) bool { return due[i].ScheduledAt.Before(*due[j].ScheduledAt) })
	q.mu.Unlock()

	for _, t := range due {
		q.runTask(t)
	}
}

// runTask transitions a single task through the pending → running →
// (done|failed) state machine. Safe to call concurrently for distinct
// tasks; the per-task transition is guarded by the registry mutex.
func (q *QueueRegistry) runTask(t *QueueTask) {
	q.mu.Lock()
	if t.Status != QueueStatusPending {
		q.mu.Unlock()
		return
	}
	t.Status = QueueStatusRunning
	q.mu.Unlock()

	txid, err := q.executor.ExecuteRecipe(t)

	q.mu.Lock()
	defer q.mu.Unlock()
	now := time.Now()
	t.ExecutedAt = &now
	if err != nil {
		t.Status = QueueStatusFailed
		t.Error = err.Error()
		log.Printf("queue task %s failed (wallet=%s): %v", t.ID, t.Wallet, err)
	} else {
		t.Status = QueueStatusDone
		t.TxID = txid
		log.Printf("queue task %s broadcast (wallet=%s tx=%s)", t.ID, t.Wallet, txid)
	}
}

// Add registers a new pending task. ScheduledAt may be nil (manual).
func (q *QueueRegistry) Add(t *QueueTask) error {
	if err := validateRecipe(t); err != nil {
		return err
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	t.ID = newQueueID()
	t.Status = QueueStatusPending
	t.CreatedAt = time.Now()
	q.tasks[t.ID] = t
	return nil
}

// Get returns a task by id (or false).
func (q *QueueRegistry) Get(id string) (*QueueTask, bool) {
	q.mu.Lock()
	defer q.mu.Unlock()
	t, ok := q.tasks[id]
	return t, ok
}

// List returns all tasks, optionally filtered by wallet name. Sorted
// by CreatedAt ascending so the queue tab displays first-queued-first
// without the frontend having to sort.
func (q *QueueRegistry) List(wallet string) []*QueueTask {
	q.mu.Lock()
	defer q.mu.Unlock()
	out := make([]*QueueTask, 0, len(q.tasks))
	for _, t := range q.tasks {
		if wallet != "" && t.Wallet != wallet {
			continue
		}
		out = append(out, t)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].CreatedAt.Before(out[j].CreatedAt) })
	return out
}

// Remove drops a task from the queue. Pending → cancellation. Done /
// failed → just clean log line. Running → caller's call (we still
// remove; the executor finishes its current call but its result is
// discarded).
func (q *QueueRegistry) Remove(id string) bool {
	q.mu.Lock()
	defer q.mu.Unlock()
	if _, ok := q.tasks[id]; !ok {
		return false
	}
	delete(q.tasks, id)
	return true
}

// RunNow flips the task to immediate execution regardless of its
// scheduled_at. Returns the post-run task or an error if it can't be
// run (already done/running/failed/missing).
func (q *QueueRegistry) RunNow(id string) (*QueueTask, error) {
	q.mu.Lock()
	t, ok := q.tasks[id]
	if !ok {
		q.mu.Unlock()
		return nil, errors.New("queue task not found")
	}
	if t.Status != QueueStatusPending {
		q.mu.Unlock()
		return nil, fmt.Errorf("queue task is %s, not pending", t.Status)
	}
	q.mu.Unlock()
	q.runTask(t)
	return t, nil
}

// FailWalletTasks marks every pending task targeting `wallet` as
// failed. Called when the wallet is being deleted — the queue can no
// longer execute those recipes, so we surface failure now rather than
// at scheduled-fire time.
func (q *QueueRegistry) FailWalletTasks(wallet, reason string) int {
	q.mu.Lock()
	defer q.mu.Unlock()
	now := time.Now()
	count := 0
	for _, t := range q.tasks {
		if t.Wallet != wallet {
			continue
		}
		if t.Status != QueueStatusPending && t.Status != QueueStatusRunning {
			continue
		}
		t.Status = QueueStatusFailed
		t.Error = reason
		t.ExecutedAt = &now
		count++
	}
	return count
}

// validateRecipe pins the contract a queued task must satisfy before
// it enters the registry. Mirrors the same rules the live send paths
// enforce so a task added today will succeed at execution time
// (modulo wallet state changes).
func validateRecipe(t *QueueTask) error {
	if t.Wallet == "" {
		return errors.New("wallet required")
	}
	hasOutputs := len(t.Outputs) > 0
	hasTo := t.To != ""
	if !hasOutputs && !hasTo {
		return errors.New("to or outputs[] required")
	}
	if hasOutputs && hasTo {
		return errors.New("to and outputs[] are mutually exclusive")
	}
	if t.SendAll && hasOutputs {
		return errors.New("send_all is incompatible with outputs[]")
	}
	if !hasOutputs && !t.SendAll && t.AmountSat <= 0 {
		return errors.New("amount_sat required (or send_all=true)")
	}
	for i, o := range t.Outputs {
		if o.Address == "" {
			return fmt.Errorf("outputs[%d].address required", i)
		}
		if o.AmountSat <= 0 {
			return fmt.Errorf("outputs[%d].amount_sat must be > 0", i)
		}
	}
	if t.FeeRate < 0 {
		return errors.New("fee_rate_sat_per_vb must be >= 0")
	}
	return nil
}

// newQueueID generates a 16-hex-character random id. Random rather
// than monotonic so the id can't be probed sequentially and so two
// tasks added the same millisecond don't collide.
func newQueueID() string {
	var b [8]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

// ParseScheduledAt accepts either a unix timestamp string ("17779...")
// or RFC3339 ("2026-05-05T18:00:00Z"). Empty input yields nil → manual
// task. Returns an error for malformed values.
func ParseScheduledAt(s string) (*time.Time, error) {
	s = trim(s)
	if s == "" {
		return nil, nil
	}
	// Numeric: unix seconds.
	if isAllDigits(s) {
		n, err := parseInt64(s)
		if err != nil {
			return nil, fmt.Errorf("invalid unix timestamp: %v", err)
		}
		// Heuristic: > 1e12 is millis (frontend Date.now()), divide.
		if n > 1e12 {
			n /= 1000
		}
		t := time.Unix(n, 0).UTC()
		return &t, nil
	}
	t, err := time.Parse(time.RFC3339, s)
	if err != nil {
		return nil, fmt.Errorf("invalid scheduled_at: %v", err)
	}
	t = t.UTC()
	return &t, nil
}

// trim is a tiny strings.TrimSpace replacement so this file doesn't
// pull in strings just for one call (keeps the import list tight).
func trim(s string) string {
	for len(s) > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r') {
		s = s[1:]
	}
	for len(s) > 0 && (s[len(s)-1] == ' ' || s[len(s)-1] == '\t' || s[len(s)-1] == '\n' || s[len(s)-1] == '\r') {
		s = s[:len(s)-1]
	}
	return s
}
func isAllDigits(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}
func parseInt64(s string) (int64, error) {
	var n int64
	for _, c := range s {
		d := int64(c - '0')
		if n > (math.MaxInt64-d)/10 {
			return 0, errors.New("overflow")
		}
		n = n*10 + d
	}
	return n, nil
}

// JSON marshalling helper used by the API handlers to keep the
// response shape identical to a single QueueTask serialization.
func tasksToJSON(tasks []*QueueTask) json.RawMessage {
	b, _ := json.Marshal(tasks)
	return b
}
