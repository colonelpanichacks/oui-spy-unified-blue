# Mega Maid Playwright UI tests

Browser smoke tests for the dashboard at `http://192.168.4.1`.

## What this catches

- Tab switching and TOOLS panel visibility (full Chrome/Firefox)
- `/api/ping` and `/api/stats` while UI is in use

## What this does not catch

- iOS/Android **captive portal** mini-browsers (test manually in Safari/Chrome after joining `megamaid`)

## Run (device on USB, Mac joins megamaid)

```bash
python3 test/run_megamaid_playwright.py
```

Already on `megamaid` WiFi:

```bash
MEGAMAID_SKIP_WIFI=1 python3 test/run_megamaid_playwright.py
```

Results: `test/megamaid_playwright_results.txt` and `test/playwright/report/`.

## Run Playwright directly

```bash
cd test/playwright
npm install
MEGAMAID_BASE=http://192.168.4.1 npx playwright test
```
