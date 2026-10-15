# AtlasLOB local chart dashboard

The optional React/FastAPI console uses the **real native `atlaslob.Engine`**.
There is no exchange, market-data feed, virtual balance, portfolio accounting or
real-money trading connection. Demo clients are labels, not authenticated accounts.

The dashboard adds execution-history and depth charts to existing order operations
and saved sessions. Matching semantics and native log formats are unchanged. The
Python/native compatibility version is now **ABI 3**: upgrade the wrapper, native
extension and frontend together, not only the static assets.

## Runtime and installation

Use the supplied local source archive's `AtlasLOB` directory or the local
`feat/local-ui` checkout. These changes are unmerged; cloning GitHub `main` alone
does not provide this dashboard. Nothing needs to be pushed or hosted to run it.

Supported UI baseline: Ubuntu 24.04 under WSL2, CPython 3.12 and a Windows browser.
Build with Node 22.12 or newer within the Node 22 line. A built wheel needs no Node
or compiler to launch.

In an elevated Windows terminal, if Ubuntu is not already installed:

~~~powershell
wsl --install --distribution Ubuntu-24.04
~~~

Complete Ubuntu's first-run user setup. Run the following inside Ubuntu as your
normal Linux user. Extract or copy the source onto the Linux filesystem; keep the
session store there too, not under `/mnt/c`.

~~~bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git python3-dev python3-venv
cd /absolute/path/to/AtlasLOB
python3 -m venv .venv-dashboard
source .venv-dashboard/bin/activate
python -m pip install -e '.[ui,ui-test]'
~~~

Install Node 22 from the [official Node.js distribution](https://nodejs.org/en/download),
then build the assets and distributables:

~~~bash
node --version                     # v22.x, at least 22.12
npm ci --prefix ui
python scripts/build_ui.py
python -m build                    # assets must be built before wheel and source archive
atlaslob-ui
~~~

Open **http://localhost:8765** in the Windows browser. This localhost smoke check
is part of installation acceptance; see
[Microsoft's WSL networking guidance](https://learn.microsoft.com/en-us/windows/wsl/networking).
If unreachable, verify WSL is running and application startup completed. Do not
bind to `0.0.0.0` or disable firewall protections.

To install the newly built compatible wheel outside the checkout:

~~~bash
python3 -m venv ~/.venvs/atlaslob-dashboard
source ~/.venvs/atlaslob-dashboard/bin/activate
python -m pip install '/absolute/path/atlaslob-0.2.1-cp312-cp312-linux_x86_64.whl[ui]'
atlaslob-ui --data-dir /home/YOUR_USER/atlaslob-dashboard-smoke --port 8767
~~~

Validate the new environment with a separate test store before stopping the prior
installation. The local package version remains `0.2.1`, so use the newly built
wheel, not an older file with the same name. An unrepaired `linux_x86_64` wheel is
for its matching Ubuntu runtime; release CI also retains the CPython-specific
manylinux matrix. Core installation stays dependency-free; importing the oracle
does not import FastAPI or Uvicorn.

After validation, stop the test and old servers. Launch the new environment
against the existing store with `atlaslob-ui`. Do not copy only frontend assets
into an ABI-2 installation.

### Routine launch and shutdown

~~~bash
atlaslob-ui
atlaslob-ui --data-dir /home/YOUR_USER/atlaslob-sessions --port 8765
~~~

Default store: `$XDG_DATA_HOME/atlaslob-ui`, falling back to
`~/.local/share/atlaslob-ui`. Use one process and one worker. An operating-system
lock rejects another owner of the same store. Ctrl+C shuts playback and the engine
down cleanly. No automatic-start service is installed.

## Dashboard walkthrough

1. An entirely new store opens **First demo**, seeded by 24 real persisted
   commands across two two-sided books. No executions have occurred yet, so its
   price chart is empty; choose **Depth** to inspect resting liquidity.
2. DEMO-A / 7 starts with bid 99 and ask 101. Buy 5 at 101 to see an execution
   candle, volume, recent trade and remaining ask quantity. DEMO-B / 42 has an
   independent book on the same global command timeline.
3. The independent Buy and Sell forms have separate type, client, quantity and
   price fields; both disable during either pending request. Limit GTC can rest;
   Limit IOC cancels residual quantity; Market IOC omits price and never rests.
   There are no optimistic book updates or automatic order retries.
4. A noncrossing Limit GTC creates an open order. Replace it to receive a new ID
   and reset priority, or cancel from its row. Both actions use that order's actual
   instrument/client. Open Orders / Events occupy the bottom tabs; recent trades
   and instrument summaries are in the right rail.
5. A native semantic rejection, such as zero quantity or a non-positive limit
   price, consumes a sequence and remains visible. Malformed HTTP input does not.
   Signed chart arithmetic does not alter the native positive-order-price policy.
6. Choose **Create Market Walk** on an empty chart, or select it under
   **Explore scenarios**. Its 600 frozen commands cover both instruments, initial
   liquidity, directional moves, sweeps, replenishment, cancel and replace.
   It starts empty and paused: Step or Play produces actual native executions.
7. The five short scenarios remain: price-time priority, multi-level sweep,
   cancel/replace, IOC/rejections and two instruments. Playback supports
   1/2/5/10 commands per second without overlapping execution or catch-up bursts.
   Pause confirms the position after any executing command finishes.
8. Restart creates a new saved run of the same frozen scenario, preserving the
   original. Scenario mode disables manual mutations even while paused. The
   library creates sandboxes, renames runs and reopens sessions. After backend
   restart it marks the most recently opened session.

The Instruments panel reports last execution, change in ticks since the first
execution, total traded quantity and open-order count. It does not fabricate
calendar returns, balances, currency prices or extra instruments.

### Reading the charts

**Price** uses global **command sequence**, not calendar time or playback speed.
Select Last 100, Last 500 (default), or All. Buckets align at command 1. The
smallest size among 1, 5, 20 and 100 commands is chosen to keep the visible range
within 500 candles. For All: 500 commands uses size 1, 501 uses size 5, and 2,501
uses size 20.

Open/close are the first/last ordered native execution in a candle's command
range; high/low are its extrema. Volume counts execution quantity once per trade,
not once per participant. Empty buckets have no candle and zero volume.
Rejections, resting-order submissions and commands for the other instrument still
advance the timeline. Single-price candles may be flat, and quiet intervals are
expected. The unfinished current bucket updates as commands execute.

The caption names bucket size and the last represented command. **Updating…**
distinguishes older displayed chart data from a newer book view. Hover or focus
the chart and use Left/Right, Home or End to inspect exact OHLC, quantity and
command ranges. Chart reads coalesce while a request is pending; they never
submit engine commands.

**Depth** plots cumulative current resting quantity against price, separately for
bids and asks. It uses the complete native snapshot, independently of the ladder's
10/20/50-level selector. It is not historical depth. Empty and one-sided books have
explicit explanations. Pointer/keyboard inspection shows exact price, level
quantity and cumulative quantity.

Engine integers cross JSON as decimal strings and use browser `BigInt`.
Session/cumulative quantities use arbitrary-precision totals; only normalized SVG
coordinates and bounded presentation values become JavaScript numbers. Exact
labels never come from rounded drawing coordinates.

The ladder defaults to 20 levels and open orders use 50-row pages. Feeds retain
the selected instrument's latest 200 events and 100 trades. The Atlas SVG logo
and Manrope font are bundled locally. Sun, moon and monitor buttons retain Light,
Dark and System preferences in browser storage. Narrow layouts stack panels and
forms; large tables scroll independently. Preserve keyboard labels, focus,
reduced motion and 200% zoom behavior when changing the dashboard.

## Autosave and full historical recovery

Each session contains `commands.atlaslog` and versioned `metadata.json`.
`library.json` records the last opened session and `.writer.lock` owns the store.
Metadata is atomically replaced and directory-synced. It stores names, timestamps,
instrument labels and frozen scenario commands/descriptions with SHA-256.
**The native log determines engine state and sequence.**

New sessions use `Engine.create_logged(..., durability="sync_each_record")`.
The UI reopens with strict verified recovery and trade collection:

~~~python
from atlaslob import Engine

engine = Engine.recover(
    "/absolute/path/to/commands.atlaslog",
    mode="verify",
    tail_policy="strict",
    durability="sync_each_record",
    collect_trades=True,
    max_recovered_trades=10_000,
)
history = engine.recovered_trades
~~~

Public collection is opt-in and defaults to disabled. `recovered_trades` is
`None` without collection, an empty tuple if enabled recovery found no executions,
or an immutable tuple of existing `TradeEvent` values ordered by command sequence
and event index. Later submissions never append to this tuple; live events arrive
through `submit()`.

Collection supports only **strict, verified, log-only recovery**. Combining it
with fast/diagnostic mode, valid-prefix recovery, `snapshot_path` or `snapshot_dir`
is rejected before files open. The maximum must be a positive integer. Exceeding
it raises `RecoveryError` rather than truncating history. Failed collection or
verification publishes neither an engine nor partial history.

Trades are collected inside the existing native execution replay after each
record verifies. No second execution replay, Python log parser or history journal
is added. Recovery-report schemas, matching semantics and native log formats are
unchanged. ABI 3 prevents mixing this Python wrapper with an older extension.
Existing saved UI sessions need no metadata/log migration.

The chart projection is rebuilt before the reopened activation is published.
Price history, volume, instrument statistics, book, open orders and scenario
position restore fully. The separate feed starts fresh with **Session reopened ·
Activity since this session was opened**. Browser refresh without reopening keeps
the active server's buffers. The scenario cursor is the recovered native
sequence, not a mutable cursor file; reopened scenarios always pause.

“Saved through command N” appears only after native submission completes
successfully. Sequenced rejections are saved too. Engine, persistence or chart
projection faults stop mutations. Never automatically resubmit an uncertain
command: strictly reopen and inspect the book/history. A recovered command may
exist even if its earlier HTTP response was lost.

Torn/corrupt logs are never repaired, truncated or resumed automatically. Failed
recovery preserves files and marks the session unavailable for that server run.
Copy the entire closed session directory before offline diagnosis. The UI exposes
no deletion, export or valid-prefix recovery action.

Limits remain 1,000 native active orders per instrument, 2,000 total and 10,000
processed commands per session. Reaching the command limit leaves the saved
dashboard inspectable; create another session to continue.

## Implementation and API

| Area | Source | Responsibility |
|---|---|---|
| Actor | `python/src/atlaslob/ui/service.py` | Single executor owns Engine, playback, capture, projection and switching |
| Projection | `python/src/atlaslob/ui/charts.py` | Exact per-command OHLC, session totals, ranges and depth conversion |
| Storage | `python/src/atlaslob/ui/store.py` | OS writer lock, safe IDs and atomic metadata |
| Contract | `python/src/atlaslob/ui/models.py`, `app.py` | Typed HTTP interface and loopback protections |
| Scenarios | `python/src/atlaslob/ui/scenarios.py` | Deterministic commands, explanations and frozen hashes |
| Browser | `ui/src/App.tsx`, `Charts.tsx`, `dashboard.css` | Workspace, dual forms, charts and responsive layout |
| Exact values | `ui/src/exact.ts`, `chartMath.ts` | Exact formatting and normalized coordinates |
| Build | `scripts/build_ui.py` | OpenAPI/type generation, stale-contract and asset checks |
| Tests | `python/tests/ui`, `python/tests/test_engine_recovered_trades.py`, `ui/tests` | Projection, native lifecycle/history and browser coverage |

The executor serializes commands, recovery, playback and cached reads. Successful
submissions capture events and native snapshots, advance the projection/feed, then
publish the view. Unchanged reads never call `snapshot()` or recompute digests.

`GET /api/v1/view` adds `instruments` and `chart_revision`. The general `revision`
also changes for naming/playback controls; `chart_revision` tracks chart data and
timeline changes. Chart responses cache independently by chart revision,
instrument, kind and range.

~~~text
GET /api/v1/chart?instrument=7&kind=price&range=500
GET /api/v1/chart?instrument=42&kind=depth&range=all
~~~

`instrument` is 7 or 42; `kind` is price or depth; `range` is 100, 500 or all
(default 500) and affects price candles only. Responses contain instance,
activation, chart revision, sequence, instrument, kind, range, bucket size and
sequence bounds, plus candles or full-book depth points. `ChartView.revision`
matches `View.chart_revision`, not `View.revision`. Activation and instance changes
invalidate old browser data. Chart errors expose a read-only retry action.

Other `/api/v1` routes remain status, sessions (list/create/rename/open), commands
(New/Cancel/Replace), scenarios and playback. `/docs` and `/openapi.json` describe
the contract. Serving is same-origin, loopback-only and single-worker; mutations
reject cross-origin requests. This is not a hosted multi-user security boundary.

## Development and verification

~~~bash
source .venv-dashboard/bin/activate
atlaslob-ui --dev                   # API-only on localhost:8765
# In another terminal, same checkout with Node 22 on PATH:
npm run dev --prefix ui             # Vite localhost:5173; /api proxy to 8765
~~~

Stop development servers before browser tests use their separate port.

~~~bash
python scripts/build_ui.py --check
npm test --prefix ui
npm run format:check --prefix ui
python -m pytest python/tests/ui python/tests/test_engine_recovered_trades.py python/tests/test_engine_api.py
python -m mypy python/src/atlaslob/ui python/tests/ui
cd ui
npx playwright install --with-deps chromium
npm run test:e2e                    # isolated temp store, separate port 8766
cd ..
python -m build
python tests/packaging/verify_sdist.py dist
~~~

The `local-ui` job in `.github/workflows/ci.yml` runs contract/frontend checks,
native UI/recovered-trade/API tests and the complete Chromium suite, then builds
and installs outside the checkout. Existing C++ correctness, replay, sanitizer
and packaging gates remain. Coverage includes exact large values, ordered
multi-fill candles, empty buckets, rejections, instrument isolation, caches,
recovered/live-history equality, corrupt logs, collection limits and slow reads.

Build assets before wheels/source archives. Packages include static assets, SVG
logos and the font; source archives include frontend source/lockfiles and these
guides. Node dependencies, browser outputs and source maps are excluded. An
installed-wheel smoke must confirm native launch/recovery outside the checkout
without Node. This guide gives the procedure, not a claim that any particular
artifact has passed it.

The sub-500-ms feedback target is a local UI objective, not an engine benchmark.
`python/tests/ui/test_acceptance.py` measures service feedback with 1,000 active
orders. Browser timing and published native benchmarks are separate.

Deferred: uploaded workloads, backward seeking, historical trade-feed restoration,
historical depth, recovery snapshots for chart collection, session deletion/export,
hosting, authentication, account/portfolio accounting, external market data,
advanced debugging and the planned Linux gateway.
