import { test, expect, type Page } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

async function newSandbox(page: Page, name: string) {
  await page.getByRole('button', { name: 'Session library', exact: true }).click();
  await page
    .getByRole('dialog', { name: 'Session library' })
    .getByRole('button', { name: 'New sandbox', exact: true })
    .click();
  await page.getByLabel('Session name', { exact: true }).fill(name);
  await page.getByRole('button', { name: 'Create sandbox' }).click();
  await expect(page.getByRole('heading', { name, exact: true })).toBeVisible();
}

test('manual walkthrough, instrument filtering, reopen and exact rejected outcomes', async ({
  page,
}) => {
  const errors: string[] = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto('/');
  await newSandbox(page, 'Browser manual');
  const buy = page.getByRole('form', { name: 'Buy order', exact: true });
  const sell = page.getByRole('form', { name: 'Sell order', exact: true });
  await buy.getByLabel('Limit price · ticks', { exact: true }).fill('100');
  await page.getByRole('button', { name: 'Submit buy order' }).click();
  await expect(page.getByRole('button', { name: 'Replace order 1', exact: true })).toBeVisible();
  await sell.getByLabel('Limit price · ticks', { exact: true }).fill('102');
  await page.getByRole('button', { name: 'Submit sell order' }).click();
  await page.getByRole('button', { name: 'Replace order 1', exact: true }).click();
  await page.getByLabel('New limit price · ticks').fill('101');
  await page.getByRole('button', { name: 'Replace and reset priority' }).click();
  await expect(page.getByRole('button', { name: 'Cancel order 3', exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Cancel order 3', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Cancel order 3', exact: true })).toHaveCount(0);
  await page.getByRole('combobox', { name: 'Instrument', exact: true }).selectOption('42');
  await expect(page.getByText('No open orders for this instrument.')).toBeVisible();
  await page.getByRole('combobox', { name: 'Instrument', exact: true }).selectOption('7');
  await expect(page.getByRole('button', { name: 'Cancel order 2', exact: true })).toBeVisible();
  await buy.getByLabel('Order type').selectOption('market_ioc');
  await expect(buy.getByLabel('Limit price · ticks', { exact: true })).toHaveCount(0);
  await expect(sell.getByLabel('Limit price · ticks', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Submit buy order' }).click();
  await expect(page.getByText('Buy 5 @ 102', { exact: true })).toBeVisible();
  await buy.getByLabel('Quantity', { exact: true }).fill('0');
  await page.getByRole('button', { name: 'Submit buy order' }).click();
  await expect(page.getByRole('alert')).toContainText('Rejected:');
  await expect(page.getByText('Saved through command 6', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Session library', exact: true }).click();
  await page.getByRole('button', { name: 'Open Browser manual', exact: true }).click();
  await expect(
    page.getByText('Session reopened · Activity since this session was opened'),
  ).toBeVisible();
  await expect(page.getByText('Buy 5 @ 102', { exact: true })).toHaveCount(0);
  await expect(page.getByText('Saved through command 6', { exact: true })).toBeVisible();
  expect(errors).toEqual([]);
});

test('scenario step, play, pause, restart and manual-mode isolation', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Explore scenarios', exact: true }).click();
  await page.getByRole('button', { name: 'Create scenario' }).click();
  await expect(page.getByText('Step 0 of 5', { exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Submit buy order' })).toBeDisabled();
  await page.getByRole('button', { name: 'Step', exact: true }).click();
  await expect(page.getByText('Step 1 of 5', { exact: true })).toBeVisible();
  await page.getByRole('combobox', { name: 'Speed', exact: true }).selectOption('1');
  await page.getByRole('button', { name: 'Play', exact: true }).click();
  await expect(page.getByText('Step 2 of 5', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Pause', exact: true }).click();
  const paused = await page.getByText(/Step \d of 5/, { exact: true }).textContent();
  await page.waitForTimeout(500);
  expect(await page.getByText(/Step \d of 5/, { exact: true }).textContent()).toBe(paused);
  await page.getByRole('button', { name: 'Restart scenario', exact: true }).click();
  await expect(page.getByText('Step 0 of 5', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Session library', exact: true }).click();
  await expect(
    page.getByRole('button', { name: 'Open Price-time priority', exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole('button', { name: 'Open Price-time priority · Restart', exact: true }),
  ).toBeVisible();
});

test('both themes, keyboard focus, narrow windows and 200% enlargement', async ({ page }) => {
  await page.goto('/');
  await newSandbox(page, 'Accessibility');
  for (const theme of ['light', 'dark']) {
    await page
      .getByRole('button', { name: `${theme === 'light' ? 'Light' : 'Dark'} theme`, exact: true })
      .click();
    await expect(page.locator('html')).toHaveAttribute('data-theme', theme);
    const results = await new AxeBuilder({ page })
      .withTags(['wcag2a', 'wcag2aa', 'wcag21aa'])
      .analyze();
    expect(results.violations).toEqual([]);
    await page.screenshot({ path: `test-results/workspace-${theme}.png`, fullPage: true });
  }
  await page.reload();
  await expect(page.getByRole('button', { name: 'Dark theme', exact: true })).toHaveAttribute(
    'aria-pressed',
    'true',
  );
  await page.getByRole('button', { name: 'Explore scenarios', exact: true }).focus();
  await page.keyboard.press('Enter');
  await expect(page.getByRole('dialog')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('button', { name: 'Explore scenarios', exact: true })).toBeFocused();
  await page.setViewportSize({ width: 390, height: 844 });
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await expect(page.getByRole('button', { name: 'Submit buy order' })).toBeEnabled();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: 'test-results/workspace-narrow.png', fullPage: true });
  await page.setViewportSize({ width: 720, height: 500 });
  await page.addStyleTag({ content: 'html { font-size: 200%; }' });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await expect(page.getByRole('button', { name: 'Submit buy order' })).toBeEnabled();
});

test('uncertain command response is not retried on reconnect', async ({ page }) => {
  await page.goto('/');
  await newSandbox(page, 'Disconnect');
  let commands = 0;
  await page.route('**/api/v1/commands', async (route) => {
    commands += 1;
    await route.fetch(); // Backend commits, but the browser never receives its response.
    await route.abort('connectionreset');
  });
  await page.getByRole('button', { name: 'Submit buy order' }).click();
  await expect(page.getByRole('alert')).toContainText('No automatic retry');
  await expect(page.getByText('Saved through command 1', { exact: true })).toBeVisible();
  await page.waitForTimeout(600);
  expect(commands).toBe(1);
  await expect(page.getByRole('button', { name: 'Cancel order 1', exact: true })).toBeVisible();
});

test('visible command feedback with 1,000 active orders', async ({ page }) => {
  test.setTimeout(120000);
  await page.goto('/');
  await newSandbox(page, 'Responsiveness');
  const initial = await (await page.request.get('/api/v1/view')).json();
  for (let i = 0; i < 1000; i++) {
    const result = await page.request.post('/api/v1/commands', {
      data: {
        activation: initial.activation,
        kind: 'new',
        instrument_id: '7',
        client_id: '1',
        side: 'buy',
        order_type: 'limit_gtc',
        quantity: '5',
        price: String(1000 + (i % 60)),
      },
    });
    expect(result.ok()).toBe(true);
  }
  await expect(page.getByText('Saved through command 1,000', { exact: true })).toBeVisible();
  const started = performance.now();
  await page.getByRole('button', { name: 'Cancel order 1', exact: true }).click();
  await expect(page.getByText('Saved through command 1,001', { exact: true })).toBeVisible();
  const elapsed = performance.now() - started;
  console.log(`1,000-order visible browser feedback: ${elapsed.toFixed(1)} ms`);
  // The requested host target is not a portable published engine benchmark gate.
  if (!process.env.CI) expect(elapsed).toBeLessThan(500);
});
