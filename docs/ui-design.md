# Chart-centered dashboard design

The dashboard follows the user's Raxon-style reference: compact Manrope typography,
neutral light/dark surfaces, restrained separators, bid/ask quantity fills and a
central chart. The earlier [order-book collection](https://dribbble.com/search/order-book)
informed the visual direction. Reference imagery is not embedded in the app.

This is functional, not presentation-only. It adds native execution-history
collection, exact projections, a chart API, a longer scenario and SVG rendering.
It does not add exchanges, market-data feeds, balances, portfolio accounting,
stop orders, currency returns or matching semantics.

## Workspace and reading order

At widths of at least 1,280 CSS pixels, 280-pixel side columns surround a flexible
center. Bids/asks are left, Price/Depth chart tabs center, and recent trades plus
instrument summaries right. Independent Buy/Sell forms sit below the chart;
Open Orders / Events span the bottom.

Below 1,280 pixels the right rail moves beneath the main workspace. Below 860
pixels panels/forms stack with the chart first. Below 561 pixels the right rail
and book sides also become single-column. Large tables scroll within panels.
Color supplements side names, position and exact-value descriptions; readable
labels and keyboard focus remain required in both themes and at zoom.

Instruments replaces the reference's Top Gainers. Last price, change since first
execution, volume and open-order count describe this session's DEMO-A / 7 and
DEMO-B / 42, not external markets.

## Charts are execution evidence

Candles/volume summarize real native trade events on a global command clock.
Last 100 / Last 500 / All select the command range. Bucket sizes 1 / 5 / 20 / 100
keep the display within 500 candles. Empty buckets remain empty, including
rejections and commands for the other instrument. Partial current buckets update
without fabricated trades.

Depth is cumulative current native-book quantity, including levels hidden by the
ladder display limit, not a historical series. Values stay decimal strings/BigInt;
only normalized SVG coordinates become floating point. No charting dependency,
remote data or fabricated price history is used.

Reopening rebuilds price history from verified recovered trades before activation,
while the recent activity feed starts fresh. The 600-command Market Walk creates
chart activity through ordinary frozen scenario commands and starts paused like
the five shorter demonstrations. See [the user guide](ui.md) for behavior.

## Brand, typography and interaction

The user's Atlas/globe silhouette is traced in `ui/public/atlaslob-logo.svg` as
vector spline paths, not an embedded bitmap. Seven paths use a `0 0 195 313`
viewBox; the original grid is removed and white regions are transparent cutouts.
`atlaslob-logo-light.svg` shares the geometry with a light fill for dark surfaces.
The original 252 × 362-pixel image limits trace detail. Transparent
1,276 × 2,048 PNG clipart was rendered from SVG.

Manrope is bundled locally. Shared spacing/color tokens remain in
`ui/src/style.css`, with chart/layout extensions in `dashboard.css`. Sun, moon
and monitor icons select Light, Dark and System; accessible names, tooltips and
browser-local preferences remain intact.

Dual forms retain independent draft fields but share a global mutation lock.
Scenario sessions disable manual mutations whether running or paused. Charts have
pointer/keyboard exact-value inspection, empty/one-sided explanations and revision
captions. Slower reads coalesce and must not block command feedback.

## Verification and local delivery

Run contract/build checks, frontend tests/format checks, Python UI/recovered-trade
tests and the complete Chromium walkthrough. Include narrow layouts, both themes,
keyboard interaction, large values, one-level depth sides, slow reads and restored
history. Existing SVG/font asset tests remain.

Install the ABI-3 native extension and Python service with the rebuilt frontend;
copying only frontend assets is insufficient. Validate a replacement environment
and separate test store before switching the local installation. Saved sessions
need no log migration and must be preserved. Nothing is pushed to GitHub or
published to hosting as part of local delivery.
