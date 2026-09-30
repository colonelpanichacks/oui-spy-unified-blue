import { defineConfig } from '@playwright/test';

const baseURL = process.env.MEGAMAID_BASE || 'http://192.168.4.1';

export default defineConfig({
  testDir: './tests',
  timeout: 60000,
  expect: { timeout: 15000 },
  use: {
    baseURL,
    headless: true,
    actionTimeout: 15000,
    navigationTimeout: 30000,
  },
  reporter: [['list'], ['html', { open: 'never', outputFolder: 'report' }]],
});
