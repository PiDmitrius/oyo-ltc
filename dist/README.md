# OYO portable stand

A self-contained Litecoin/OYO node (`litecoind`) plus the `oyo-web` wallet &
explorer. Unpack anywhere and run one script — the folder you unpack into is the
portable root; all data is created in subfolders under it.

```
oyo-portable-<version>/
├─ oyo.sh          installer / manager
├─ README.md
├─ VERSION
├─ node/litecoind  the node
└─ web/oyo-web     the wallet + explorer
```

## Requirements

- Linux x86-64 with glibc 2.29+ / libstdc++ 3.4.26+ — i.e. Ubuntu 20.04+,
  Debian 11+ (measured from the binaries' versioned-symbol floor; both are
  linked inside an Ubuntu 20.04 build container, so the floor is reproducible).
- A user systemd session (`systemctl --user`). No other packages: both binaries
  depend only on the base C/C++ runtime (`libc`, `libstdc++`, `libgcc`, `libm`).
- `curl` (used to probe an external node in `web` mode).

## Quick start

```sh
tar xzf oyo-portable-<version>-linux-x86_64.tar.gz
cd oyo-portable-<version>

# everything on this machine (node + web):
./oyo.sh full mainnet

# only the web UI, against a node you already run elsewhere:
./oyo.sh web mainnet --rpc http://127.0.0.1:9332 --rpc-user oyo --rpc-pass SECRET

# only the node (web runs on another host):
./oyo.sh node mainnet
```

On first run it prints the web URL + login (also saved in
`<network>/oyo-web.json`). Re-running `up` never regenerates anything — it just
ensures the services are current and running.

## Components & networks

| component | installs                | RPC credentials                      |
|-----------|-------------------------|--------------------------------------|
| `full`    | node + web (this host)  | generated, shared node↔web (localhost) |
| `web`     | web only → external node| you provide via `--rpc[-user/-pass]` |
| `node`    | node only               | generated, printed for a remote web  |

Networks: `mainnet` · `testnet` · `regtest`. Each is an independent stand
(own subfolder, own ports, own services) — they can coexist.

## Manage

```sh
./oyo.sh <component> <network> status     # state + URL + login + node tip
./oyo.sh <component> <network> logs        # follow the service log
./oyo.sh <component> <network> restart
./oyo.sh <component> <network> update      # after replacing the binaries with a newer release
./oyo.sh <component> <network> down        # stop + disable (keeps data)
```

## Where things live

```
<root>/<network>/
├─ litecoin.conf   node config        (full / node)
├─ oyo-web.json    web config + creds  (full / web)
├─ chain/          node datadir        (full / node)
└─ mirror/         oyo-web LevelDB mirror
```

To repoint web at a different node: edit `<network>/oyo-web.json`, then
`./oyo.sh web <network> restart`. To wipe a stand: `… down` then remove the
`<network>/` folder.

## Notes

- `web` mode preflights the external node: reachable, credentials valid, chain
  matches, and `txindex` enabled (oyo-web needs it for transaction lookups).
- The node binds RPC to `127.0.0.1` only. To drive `oyo-web` from another host,
  open RPC deliberately (`rpcallowip`/`rpcbind` in `litecoin.conf`) or, preferred,
  use an SSH tunnel.
- Moving the folder: re-run the same `up` afterwards so the service paths update.
