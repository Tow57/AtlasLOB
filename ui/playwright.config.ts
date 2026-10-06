import { defineConfig } from '@playwright/test';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const store = mkdtempSync(join(tmpdir(), 'atlaslob-browser-'));
export default defineConfig({
  testDir: './tests',
  workers: 1,
  retries: 0,
  timeout: 30000,
  use: {
    baseURL: 'http://127.0.0.1:8766',
    viewport: { width: 1440, height: 1000 },
    trace: 'retain-on-failure',
  },
  webServer: {
    command: `python -m atlaslob.ui --port 8766 --data-dir ${store}`,
    url: 'http://127.0.0.1:8766/api/v1/status',
    reuseExistingServer: false,
  },
});
