import { test, expect } from '@playwright/test';

test.describe('Mega Maid dashboard', () => {
  test('loads home and API ping', async ({ page, request }) => {
    const ping = await request.get('/api/ping');
    expect(ping.ok()).toBeTruthy();
    expect(await ping.text()).toBe('ok');

    await page.goto('/');
    await expect(page.locator('#modeBadge')).toBeVisible();
    await expect(page.locator('#tabBar')).toBeVisible();
  });

  test('tab buttons switch panels', async ({ page }) => {
    await page.goto('/');

    const live = page.locator('#p0');
    const prev = page.locator('#p1');
    const ouis = page.locator('#p2');
    const tools = page.locator('#p3');

    await expect(live).toHaveClass(/a/);

    await page.locator('#tabBar button[data-tab="1"]').click();
    await expect(prev).toHaveClass(/a/);
    await expect(live).not.toHaveClass(/a/);

    await page.locator('#tabBar button[data-tab="2"]').click();
    await expect(ouis).toHaveClass(/a/);
    await expect(prev).not.toHaveClass(/a/);

    await page.locator('#tabBar button[data-tab="3"]').click();
    await expect(tools).toHaveClass(/a/);
    await expect(page.locator('#exportSummary')).toBeVisible();

    await page.locator('#tabBar button[data-tab="0"]').click();
    await expect(live).toHaveClass(/a/);
    await expect(tools).not.toHaveClass(/a/);
  });

  test('stats API returns dashboard profile', async ({ request }) => {
    const res = await request.get('/api/stats');
    expect(res.ok()).toBeTruthy();
    const stats = await res.json();
    expect(stats.radio_profile).toBe('dashboard');
    expect(typeof stats.total).toBe('number');
    expect(typeof stats.prev_session_count).toBe('number');
  });

  test('TOOLS tab buttons are present', async ({ page }) => {
    await page.goto('/');
    await page.locator('#tabBar button[data-tab="3"]').click();
    await expect(page.getByText('DOWNLOAD JSON').first()).toBeVisible();
    await expect(page.getByText('COPY JSON').first()).toBeVisible();
    await expect(page.getByText('PRIOR SESSION')).toBeVisible();
  });
});
