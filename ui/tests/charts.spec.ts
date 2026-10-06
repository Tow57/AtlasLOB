import { test, expect } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

test('Market Walk charts, keyboard inspection, full recovery and instrument depth', async ({
  page,
}) => {
  test.setTimeout(180000);
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  const old = await (await page.request.get('/api/v1/view')).json();
  const created = await (
    await page.request.post('/api/v1/sessions', {
      data: {
        activation: old.activation,
        name: 'Chart recovery',
        mode: 'scenario',
        scenario_id: 'market-walk',
      },
    })
  ).json();
  expect(created.view.playback.cursor).toBe(0);
  await page.goto('/');
  await expect(page.getByText('Step 0 of 600', { exact: true })).toBeVisible();
  await expect(page.getByText('No executions in this range', { exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Submit sell order' })).toBeDisabled();
  for (let i = 0; i < 600; i++) {
    const response = await page.request.post('/api/v1/playback', {
      data: { activation: created.view.activation, action: 'step' },
    });
    expect(response.ok()).toBe(true);
  }
  await expect(page.getByText('Step 600 of 600', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'All', exact: true }).click();
  await expect(
    page.getByText('5 command(s) per candle · No calendar timestamps', { exact: true }),
  ).toBeVisible();
  const chart = page.getByRole('group', {
    name: 'Price chart; use left and right arrows to inspect candles',
    exact: true,
  });
  await chart.focus();
  await page.keyboard.press('Home');
  await expect(page.locator('.chart-inspector')).toContainText('Commands 1–5. No executions.');
  await page.keyboard.press('ArrowRight');
  await expect(page.locator('.chart-inspector')).toContainText('Commands 6–10. Open 101');
  for (const theme of ['Light', 'Dark']) {
    await page.getByRole('button', { name: `${theme} theme`, exact: true }).click();
    const a11y = await new AxeBuilder({ page })
      .withTags(['wcag2a', 'wcag2aa', 'wcag21aa'])
      .analyze();
    expect(a11y.violations).toEqual([]);
    await page.screenshot({
      path: `test-results/chart-${theme.toLowerCase()}.png`,
      fullPage: true,
    });
  }
  const before = await (await page.request.get('/api/v1/chart?range=all')).json();
  const summaryBefore = await (await page.request.get('/api/v1/view')).json();
  await page.getByRole('button', { name: 'Session library', exact: true }).click();
  await page.getByRole('button', { name: 'Open Chart recovery', exact: true }).click();
  await expect(
    page.getByText('Session reopened · Activity since this session was opened', { exact: true }),
  ).toBeVisible();
  const after = await (await page.request.get('/api/v1/chart?range=all')).json();
  expect(after.candles).toEqual(before.candles);
  expect(after.activation).not.toBe(before.activation);
  const reopenedView = await (await page.request.get('/api/v1/view')).json();
  expect(reopenedView.instruments).toEqual(summaryBefore.instruments);
  expect(reopenedView.trades).toEqual([]);
  await page.getByRole('button', { name: 'Depth', exact: true }).click();
  await expect(
    page.getByRole('img', { name: 'Cumulative bid and ask depth across the full book' }),
  ).toBeVisible();
  await page.getByRole('button', { name: 'Select DEMO-B', exact: true }).click();
  await page.getByRole('button', { name: 'Depth', exact: true }).click();
  await expect(page.locator('.chart-inspector')).toContainText('Bid at 180');
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: 'test-results/chart-mobile.png', fullPage: true });
  expect(errors).toEqual([]);
});

test('chart rejects delayed responses from a previous activation', async ({ page }) => {
  const old = await (await page.request.get('/api/v1/view')).json();
  const first = await (
    await page.request.post('/api/v1/sessions', {
      data: { activation: old.activation, name: 'Stale chart A', mode: 'manual' },
    })
  ).json();
  let release!: () => void;
  let intercepted!: () => void;
  const ready = new Promise<void>((resolve) => {
    intercepted = resolve;
  });
  const gate = new Promise<void>((resolve) => {
    release = resolve;
  });
  let delayed = false;
  await page.route('**/api/v1/chart?**', async (route) => {
    if (delayed) return route.continue();
    delayed = true;
    const response = await route.fetch();
    intercepted();
    await gate;
    await route.fulfill({ response });
  });
  await page.goto('/');
  await ready;
  await page.request.post('/api/v1/sessions', {
    data: { activation: first.view.activation, name: 'Stale chart B', mode: 'manual' },
  });
  await expect(page.getByRole('heading', { name: 'Stale chart B', exact: true })).toBeVisible();
  release();
  await expect(page.getByText('No executions in this range', { exact: true })).toBeVisible();
  await expect(page.getByText('Through #0', { exact: true })).toBeVisible();
  await expect(page.getByRole('alert')).toHaveCount(0);
});

test('slow chart reads make progress during playback and settle after pause', async ({ page }) => {
  const old = await (await page.request.get('/api/v1/view')).json();
  await page.request.post('/api/v1/sessions', {
    data: {
      activation: old.activation,
      name: 'Slow chart',
      mode: 'scenario',
      scenario_id: 'market-walk',
    },
  });
  let active = 0;
  let peak = 0;
  await page.route('**/api/v1/chart?**', async (route) => {
    active += 1;
    peak = Math.max(peak, active);
    const response = await route.fetch();
    await new Promise((resolve) => setTimeout(resolve, 700));
    await route.fulfill({ response });
    active -= 1;
  });
  await page.goto('/');
  await page.getByRole('combobox', { name: 'Speed', exact: true }).selectOption('10');
  await page.getByRole('button', { name: 'Play', exact: true }).click();
  await expect(page.locator('.chart-caption')).toContainText(/Through #[1-9]/, { timeout: 6000 });
  await expect(page.getByRole('button', { name: 'Pause', exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Pause', exact: true }).click();
  const paused = await (await page.request.get('/api/v1/view')).json();
  await expect(page.locator('.chart-caption')).toContainText(`Through #${paused.sequence}`);
  await expect(page.locator('.chart-caption')).not.toContainText('Updating');
  expect(peak).toBe(1);
});

test('single-level depth remains visible and dual forms share pending protection', async ({
  page,
}) => {
  const old = await (await page.request.get('/api/v1/view')).json();
  await page.request.post('/api/v1/sessions', {
    data: { activation: old.activation, name: 'One level', mode: 'manual' },
  });
  await page.goto('/');
  await page.getByRole('button', { name: 'Depth', exact: true }).click();
  await expect(page.getByText('No resting liquidity', { exact: true })).toBeVisible();
  let release!: () => void;
  let intercepted!: () => void;
  const ready = new Promise<void>((resolve) => {
    intercepted = resolve;
  });
  const gate = new Promise<void>((resolve) => {
    release = resolve;
  });
  await page.route('**/api/v1/commands', async (route) => {
    const response = await route.fetch();
    intercepted();
    await gate;
    await route.fulfill({ response });
  });
  await page.getByRole('button', { name: 'Submit buy order' }).click();
  await ready;
  await expect(page.getByRole('button', { name: 'Submit buy order' })).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Submit sell order' })).toBeDisabled();
  release();
  await expect(page.locator('.depth-buy .depth-level-marker')).toHaveCount(1);
  await expect(page.getByRole('button', { name: 'Submit sell order' })).toBeEnabled();
  await page.unroute('**/api/v1/commands');
  await page
    .getByRole('form', { name: 'Sell order', exact: true })
    .getByLabel('Limit price · ticks', { exact: true })
    .fill('102');
  await page.getByRole('button', { name: 'Submit sell order' }).click();
  await expect(page.locator('.depth-sell .depth-level-marker')).toHaveCount(1);
  const depth = page.getByRole('group', {
    name: 'Depth chart; use left and right arrows to inspect levels',
    exact: true,
  });
  await depth.focus();
  await page.keyboard.press('Home');
  await expect(page.locator('.chart-inspector')).toContainText('Bid at 100');
  await page.keyboard.press('End');
  await expect(page.locator('.chart-inspector')).toContainText('Ask at 102');
});
