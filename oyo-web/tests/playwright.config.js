const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: '.',
  testMatch: '*.spec.js',
  timeout: 30000,
  retries: 0,
  // Tests share a single regtest node — running describes in parallel
  // across workers races on chain state (mining, peg-ins, shared
  // wallets like test-e2e). Force single-worker so all tests run
  // sequentially against deterministic chain state.
  workers: 1,
  use: {
    baseURL: process.env.OYO_URL || 'http://127.0.0.1:8880',
    headless: true,
    screenshot: 'only-on-failure',
  },
  reporter: [['list']],
});
