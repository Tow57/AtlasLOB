import { useCallback, useEffect, useRef, useState, type FormEvent, type ReactNode } from 'react';
import {
  request,
  type Action,
  type Command,
  type Order,
  type Scenario,
  type Sessions,
  type View,
} from './api';
import { barPercent, format, validInteger } from './exact';
import { Charts } from './Charts';

type Theme = 'system' | 'light' | 'dark';
type Mutate = (path: string, body: object) => Promise<boolean>;

function Modal({
  title,
  close,
  children,
}: {
  title: string;
  close: () => void;
  children: ReactNode;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null;
    dialog.current?.showModal();
    return () => {
      dialog.current?.close();
      previous?.focus();
    };
  }, []);
  return (
    <dialog
      ref={dialog}
      onCancel={(e) => {
        e.preventDefault();
        close();
      }}
      aria-label={title}
    >
      <div className="section-head">
        <h2>{title}</h2>
        <button onClick={close} aria-label="Close dialog">
          Close
        </button>
      </div>
      {children}
    </dialog>
  );
}

export function App() {
  const [view, setView] = useState<View | null>(null);
  const current = useRef<View | null>(null);
  const [instrument, setInstrument] = useState('7');
  const [depth, setDepth] = useState(20);
  const [page, setPage] = useState(1);
  const [bottomTab, setBottomTab] = useState<'orders' | 'events'>('orders');
  const [connected, setConnected] = useState(false);
  const [pending, setPending] = useState(false);
  const busy = useRef(false);
  const generation = useRef(0);
  const [notice, setNotice] = useState({ text: '', error: false });
  const [library, setLibrary] = useState(false);
  const [sessions, setSessions] = useState<Sessions>({ sessions: [], last_opened: null });
  const [scenarios, setScenarios] = useState<Scenario[]>([]);
  const [creation, setCreation] = useState<'manual' | 'scenario' | null>(null);
  const [rename, setRename] = useState<{ id: string; name: string } | null>(null);
  const [theme, setTheme] = useState<Theme>(() => {
    try {
      const saved = localStorage.getItem('atlaslob-theme');
      return saved === 'light' || saved === 'dark' ? saved : 'system';
    } catch {
      return 'system';
    }
  });
  const accept = useCallback((next: View) => {
    const old = current.current;
    if (old?.instance_id === next.instance_id && old.revision > next.revision) return;
    current.current = next;
    setView(next);
    setConnected(true);
  }, []);
  const query = `/view?instrument=${instrument}&depth=${depth}&page=${page}`;
  const refresh = useCallback(async () => {
    const ticket = generation.current;
    const next = await request<View>(query);
    if (ticket === generation.current) accept(next);
  }, [query, accept]);
  const loadLibrary = useCallback(async () => {
    try {
      setSessions(await request<Sessions>('/sessions'));
    } catch (e) {
      setNotice({ text: String(e), error: true });
    }
  }, []);

  useEffect(() => {
    const media = matchMedia('(prefers-color-scheme: dark)');
    const apply = () => {
      document.documentElement.dataset.theme =
        theme === 'system' ? (media.matches ? 'dark' : 'light') : theme;
    };
    apply();
    media.addEventListener('change', apply);
    try {
      localStorage.setItem('atlaslob-theme', theme);
    } catch {
      /* private storage can be disabled */
    }
    return () => media.removeEventListener('change', apply);
  }, [theme]);

  useEffect(() => {
    let stopped = false;
    let timer: ReturnType<typeof setTimeout>;
    generation.current += 1;
    const poll = async () => {
      if (!stopped && !busy.current && document.visibilityState === 'visible') {
        try {
          await refresh();
        } catch {
          if (!stopped) setConnected(false);
        }
      }
      if (!stopped) timer = setTimeout(poll, 250);
    };
    void poll();
    return () => {
      stopped = true;
      clearTimeout(timer);
      generation.current += 1;
    };
  }, [refresh]);

  useEffect(() => {
    request<Scenario[]>('/scenarios')
      .then(setScenarios)
      .catch(() => {});
  }, []);
  useEffect(() => {
    if (library || (view && !view.session)) void loadLibrary();
  }, [library, view?.activation, loadLibrary]);

  const mutate: Mutate = async (path, body) => {
    if (busy.current) return false;
    busy.current = true;
    setPending(true);
    generation.current += 1;
    try {
      const result = await request<Action>(path, {
        ...body,
        activation: current.current?.activation ?? null,
      });
      // Mutations may change activation, but no earlier poll may overwrite this response.
      if (result.view.instrument_id === instrument && depth === 20 && page === 1)
        accept(result.view);
      else {
        current.current = result.view;
        await refresh();
      }
      setNotice({
        text: result.message,
        error: result.outcome === 'fault' || result.outcome === 'rejected',
      });
      await loadLibrary();
      return result.outcome !== 'fault';
    } catch (e) {
      setNotice({
        text: `${String(e)} No automatic retry was made. Refreshing authoritative state; inspect it before submitting again.`,
        error: true,
      });
      try {
        await refresh();
      } catch {
        setConnected(false);
      }
      return false;
    } finally {
      busy.current = false;
      setPending(false);
    }
  };

  const disabled =
    pending ||
    !connected ||
    !!view?.fault ||
    !!view?.command_limit_reached ||
    view?.instrument_id !== instrument;
  const manual = view?.session?.mode === 'manual';
  const libraryBody = (
    <>
      <div className="library-intro">
        <p>
          Each session is its own saved matching engine. Reopen a book or start a fresh experiment.
        </p>
        <div className="actions">
          <button
            className="primary"
            disabled={pending || !connected}
            onClick={() => setCreation('manual')}
          >
            New sandbox
          </button>
          <button disabled={pending || !connected} onClick={() => setCreation('scenario')}>
            Explore scenarios
          </button>
        </div>
      </div>
      <div className="session-list">
        {sessions.sessions.map((s) => (
          <article className="session-row" key={s.id}>
            <div>
              <h3>{s.name}</h3>
              <p>
                {s.mode === 'manual' ? 'Manual sandbox' : 'Scenario run'}
                {sessions.last_opened === s.id ? ' · Most recently opened' : ''}
              </p>
              <small>
                Created {date(s.created_at)} · Last activity {date(s.updated_at)}
              </small>
              {s.error && <p className="error-text">{s.error}</p>}
            </div>
            <div className="actions">
              <button
                disabled={pending || !connected}
                onClick={() => {
                  void mutate(`/sessions/${s.id}/open`, {}).then((ok) => {
                    if (ok) setLibrary(false);
                  });
                }}
              >
                Open {s.name}
              </button>
              <button
                disabled={pending}
                onClick={() => setRename({ id: s.id, name: s.name })}
                aria-label={`Rename ${s.name}`}
              >
                Rename
              </button>
            </div>
          </article>
        ))}
        {sessions.sessions.length === 0 && <p>No saved sessions yet.</p>}
      </div>
    </>
  );

  return (
    <>
      <a href="#workspace" className="skip-link">
        Skip to workspace
      </a>
      <header className="app-header">
        <div className="brand">
          <img
            className="brand-mark logo-dark"
            src="/atlaslob-logo.svg"
            alt=""
            width="34"
            height="54"
          />
          <img
            className="brand-mark logo-light"
            src="/atlaslob-logo-light.svg"
            alt=""
            width="34"
            height="54"
          />
          <div>
            <strong>AtlasLOB</strong>
            <span>Matching workspace</span>
          </div>
        </div>
        <div className="header-status">
          <span className={`status-dot ${connected ? 'online' : ''}`} />
          {connected ? 'Local engine connected' : 'Disconnected · reconnecting'}
        </div>
        <div className="theme-control segmented" role="group" aria-label="Theme">
          {(['light', 'dark', 'system'] as const).map((choice) => (
            <button
              key={choice}
              type="button"
              aria-label={`${choice[0].toUpperCase()}${choice.slice(1)} theme`}
              title={`${choice[0].toUpperCase()}${choice.slice(1)} theme`}
              aria-pressed={theme === choice}
              onClick={() => setTheme(choice)}
            >
              <svg
                width="18"
                height="18"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                strokeWidth="1.6"
                strokeLinecap="round"
                strokeLinejoin="round"
                aria-hidden="true"
                focusable="false"
              >
                {choice === 'light' ? (
                  <>
                    <circle cx="12" cy="12" r="4" />
                    <path d="M12 2v2m0 16v2M2 12h2m16 0h2M4.93 4.93l1.42 1.42m11.3 11.3 1.42 1.42M4.93 19.07l1.42-1.42m11.3-11.3 1.42-1.42" />
                  </>
                ) : choice === 'dark' ? (
                  <path d="M20.8 13.2A9 9 0 0 1 10.8 3.2a9 9 0 1 0 10 10Z" />
                ) : (
                  <>
                    <rect x="3" y="4" width="18" height="13" rx="2" />
                    <path d="M8 21h8m-4-4v4" />
                  </>
                )}
              </svg>
            </button>
          ))}
        </div>
        <button
          onClick={() => {
            setLibrary(true);
            void loadLibrary();
          }}
        >
          Session library
        </button>
      </header>
      <main id="workspace">
        {notice.text && (
          <div
            className={`notice ${notice.error ? 'error' : ''}`}
            role={notice.error ? 'alert' : 'status'}
          >
            <span>{notice.text}</span>
            <button
              onClick={() => setNotice({ text: '', error: false })}
              aria-label="Dismiss notification"
            >
              Dismiss
            </button>
          </div>
        )}
        {!connected && (
          <div className="notice error" role="status">
            Orders are disabled while disconnected. Requests are never automatically resubmitted.
          </div>
        )}
        {view?.fault && (
          <div className="notice error" role="alert">
            <span>{view.fault}</span>
            {view.session && (
              <button
                disabled={pending}
                onClick={() => void mutate(`/sessions/${view.session!.id}/open`, {})}
              >
                Reopen with strict recovery
              </button>
            )}
          </div>
        )}
        {view?.command_limit_reached && (
          <div className="notice">
            This session reached 10,000 commands. Its saved book remains available to inspect. Start
            a new sandbox to continue.
          </div>
        )}
        {!view ? (
          <p className="loading">Connecting to the local engine…</p>
        ) : !view.session ? (
          <section className="library-page">
            <p className="eyebrow">YOUR EXPERIMENTS</p>
            <h1>Session library</h1>
            {libraryBody}
          </section>
        ) : (
          <>
            <div className="workspace-heading">
              <div>
                <p className="eyebrow">{manual ? 'MANUAL SANDBOX' : 'GUIDED SCENARIO'}</p>
                <h1>{view.session.name}</h1>
              </div>
              <div className="workspace-tools">
                <span className="save-status" role="status">
                  {pending ? 'Processing…' : `Saved through command ${format(view.saved_sequence)}`}
                </span>
                <button
                  disabled={pending}
                  onClick={() => setCreation(manual ? 'scenario' : 'manual')}
                >
                  {manual ? 'Explore scenarios' : 'New sandbox'}
                </button>
              </div>
            </div>
            <div className="metrics">
              <div className="instrument-metric">
                <span className="instrument-mark" aria-hidden="true">
                  {instrument === '7' ? 'A' : 'B'}
                </span>
                <label>
                  Instrument
                  <select
                    value={instrument}
                    onChange={(e) => {
                      generation.current += 1;
                      setInstrument(e.target.value);
                      setPage(1);
                    }}
                  >
                    <option value="7">DEMO-A · 7</option>
                    <option value="42">DEMO-B · 42</option>
                  </select>
                </label>
              </div>
              <Metric label="Best bid" value={format(view.bids[0]?.price)} className="buy" />
              <Metric label="Best ask" value={format(view.asks[0]?.price)} className="sell" />
              <Metric label="Spread · ticks" value={format(view.spread)} />
              <Metric
                label="Open orders"
                value={String(view.active_count)}
                detail={`${view.total_active_count} across both instruments`}
              />
              <Metric
                label="Command sequence"
                value={format(view.sequence)}
                detail="Shared global timeline"
              />
            </div>
            {!manual && (
              <ScenarioControls view={view} disabled={disabled} pending={pending} mutate={mutate} />
            )}
            <div className="workspace-grid">
              <section className="panel book-panel" aria-label="Order book">
                <div className="section-head">
                  <div>
                    <h2>Order book</h2>
                    <p>Exact integer ticks · Price increment 1</p>
                  </div>
                  <label>
                    Levels
                    <select
                      value={depth}
                      onChange={(e) => {
                        generation.current += 1;
                        setDepth(Number(e.target.value));
                      }}
                    >
                      <option>10</option>
                      <option>20</option>
                      <option>50</option>
                    </select>
                  </label>
                </div>
                <Book view={view} />
              </section>
              <Charts
                key={`${view.instance_id}:${view.activation}:${view.instrument_id}`}
                view={view}
                disabled={pending || !connected}
                createDemo={() =>
                  void mutate('/sessions', {
                    name: 'Market Walk',
                    mode: 'scenario',
                    scenario_id: 'market-walk',
                  })
                }
              />
              <section className="panel entry-panel">
                <div className="section-head">
                  <h2>Order entry</h2>
                  <span className="pill">{manual ? 'Manual' : 'Read only'}</span>
                </div>
                <div className="dual-entry">
                  {(['buy', 'sell'] as const).map((side) => (
                    <OrderEntry
                      key={`${view.activation}:${instrument}:${side}`}
                      side={side}
                      instrument={instrument}
                      disabled={disabled || !manual}
                      mutate={mutate}
                    />
                  ))}
                </div>
                {!manual && (
                  <p className="muted">
                    Scenario mode owns the command timeline. Create a new sandbox for manual orders.
                  </p>
                )}
              </section>
              <section className="panel orders-panel">
                <div className="bottom-tabs segmented" role="group" aria-label="Session detail">
                  <button
                    aria-pressed={bottomTab === 'orders'}
                    onClick={() => setBottomTab('orders')}
                  >
                    Open Orders
                  </button>
                  <button
                    aria-pressed={bottomTab === 'events'}
                    onClick={() => setBottomTab('events')}
                  >
                    Events
                  </button>
                </div>
                {bottomTab === 'orders' ? (
                  <Orders
                    view={view}
                    disabled={disabled || !manual}
                    setPage={setPage}
                    mutate={mutate}
                  />
                ) : (
                  <Activity view={view} tab="events" />
                )}
              </section>
              <aside className="right-rail" aria-label="Session market activity">
                <section className="panel activity-panel">
                  <Activity view={view} tab="trades" />
                </section>
                <section className="panel instruments-panel">
                  <div className="section-head">
                    <h2>Instruments</h2>
                    <span className="pill">Simulation</span>
                  </div>
                  {view.instruments.map((item) => (
                    <button
                      key={item.instrument_id}
                      className="instrument-card"
                      aria-label={`Select ${item.name}`}
                      aria-pressed={instrument === item.instrument_id}
                      onClick={() => {
                        generation.current += 1;
                        setInstrument(item.instrument_id);
                        setPage(1);
                      }}
                    >
                      <span className="instrument-title">
                        <span className="instrument-mark" aria-hidden="true">
                          {item.instrument_id === '7' ? 'A' : 'B'}
                        </span>
                        <strong>{item.name}</strong>
                        <span>{format(item.last_price)}</span>
                      </span>
                      <span className="instrument-change">
                        Since first execution{' '}
                        <span
                          className={
                            item.change != null && BigInt(item.change) < 0n ? 'sell' : 'buy'
                          }
                        >
                          {item.change != null && BigInt(item.change) > 0n ? '+' : ''}
                          {format(item.change)} ticks
                        </span>
                      </span>
                      <span className="instrument-detail">
                        Traded {format(item.volume)} · {item.active_count} open orders
                      </span>
                    </button>
                  ))}
                </section>
              </aside>
            </div>
            <footer>
              Synthetic instruments only · Local demonstration console · No live-market connection
              or account ownership
            </footer>
          </>
        )}
      </main>
      {library && (
        <Modal title="Session library" close={() => setLibrary(false)}>
          {libraryBody}
        </Modal>
      )}
      {creation && (
        <Modal
          title={creation === 'manual' ? 'New sandbox' : 'Explore scenarios'}
          close={() => setCreation(null)}
        >
          <CreateForm
            mode={creation}
            scenarios={scenarios}
            pending={pending}
            onCreate={async (body) => {
              const ok = await mutate('/sessions', body);
              if (ok) {
                setCreation(null);
                setLibrary(false);
                setPage(1);
              }
            }}
          />
        </Modal>
      )}
      {rename && (
        <Modal title="Rename session" close={() => setRename(null)}>
          <form
            onSubmit={(e) => {
              e.preventDefault();
              if (busy.current) return;
              busy.current = true;
              setPending(true);
              void request(`/sessions/${rename.id}`, { name: rename.name }, 'PATCH')
                .then(async () => {
                  setRename(null);
                  await loadLibrary();
                  await refresh();
                })
                .catch((e) => setNotice({ text: String(e), error: true }))
                .finally(() => {
                  busy.current = false;
                  setPending(false);
                });
            }}
          >
            <label>
              Session name
              <input
                autoFocus
                value={rename.name}
                required
                maxLength={80}
                onChange={(e) => setRename({ ...rename, name: e.target.value })}
              />
            </label>
            <button className="primary" disabled={pending}>
              Rename session
            </button>
          </form>
        </Modal>
      )}
    </>
  );
}

function Metric({
  label,
  value,
  detail,
  className = '',
}: {
  label: string;
  value: string;
  detail?: string;
  className?: string;
}) {
  return (
    <div className="metric">
      <span>{label}</span>
      <strong className={className}>{value}</strong>
      {detail && <small>{detail}</small>}
    </div>
  );
}

function Book({ view }: { view: View }) {
  const maximum = [...view.bids, ...view.asks].reduce(
    (max, l) => (BigInt(l.quantity) > max ? BigInt(l.quantity) : max),
    0n,
  );
  return (
    <div className="ladder">
      <div className="book-side" tabIndex={0} role="region" aria-label="Scrollable bids">
        <h3 className="buy">
          Bids <span>Buy orders</span>
        </h3>
        <table>
          <thead>
            <tr>
              <th>Orders</th>
              <th>Quantity</th>
              <th>Price</th>
            </tr>
          </thead>
          <tbody>
            {view.bids.map((l) => (
              <tr
                key={l.price}
                className="bid-row"
                style={{ '--bar': `${barPercent(l.quantity, maximum)}%` } as React.CSSProperties}
              >
                <td>{l.count}</td>
                <td>{format(l.quantity)}</td>
                <td className="buy">{format(l.price)}</td>
              </tr>
            ))}
          </tbody>
        </table>
        {!view.bids.length && <p className="empty">No resting bids</p>}
      </div>
      <div className="book-side" tabIndex={0} role="region" aria-label="Scrollable asks">
        <h3 className="sell">
          Asks <span>Sell orders</span>
        </h3>
        <table>
          <thead>
            <tr>
              <th>Price</th>
              <th>Quantity</th>
              <th>Orders</th>
            </tr>
          </thead>
          <tbody>
            {view.asks.map((l) => (
              <tr
                key={l.price}
                className="ask-row"
                style={{ '--bar': `${barPercent(l.quantity, maximum)}%` } as React.CSSProperties}
              >
                <td className="sell">{format(l.price)}</td>
                <td>{format(l.quantity)}</td>
                <td>{l.count}</td>
              </tr>
            ))}
          </tbody>
        </table>
        {!view.asks.length && <p className="empty">No resting asks</p>}
      </div>
      <p className="book-caption">
        {view.bids.length} of {view.bid_levels} bid levels · {view.asks.length} of {view.ask_levels}{' '}
        ask levels · Bars show relative aggregate quantity
      </p>
    </div>
  );
}

function OrderEntry({
  side,
  instrument,
  disabled,
  mutate,
}: {
  side: 'buy' | 'sell';
  instrument: string;
  disabled: boolean;
  mutate: Mutate;
}) {
  const [type, setType] = useState<'limit_gtc' | 'limit_ioc' | 'market_ioc'>('limit_gtc');
  const [quantity, setQuantity] = useState('5');
  const [price, setPrice] = useState(instrument === '7' ? '100' : '200');
  const [client, setClient] = useState('1');
  const [error, setError] = useState('');
  const submit = (e: FormEvent) => {
    e.preventDefault();
    if (!validInteger(quantity) || (type !== 'market_ioc' && !validInteger(price, true))) {
      setError('Use whole decimal integers without commas or exponents.');
      return;
    }
    setError('');
    const command: Omit<Extract<Command, { kind: 'new' }>, 'activation'> = {
      kind: 'new',
      instrument_id: instrument,
      client_id: client,
      side,
      order_type: type,
      quantity,
      ...(type === 'market_ioc' ? {} : { price }),
    };
    void mutate('/commands', command);
  };
  return (
    <form onSubmit={submit} aria-label={`${side === 'buy' ? 'Buy' : 'Sell'} order`}>
      <fieldset disabled={disabled}>
        <legend className={`order-side-title ${side}`}>
          {side === 'buy' ? 'Buy' : 'Sell'} {instrument === '7' ? 'DEMO-A' : 'DEMO-B'}
        </legend>
        <div className="form-pair">
          <label>
            Order type
            <select value={type} onChange={(e) => setType(e.target.value as typeof type)}>
              <option value="limit_gtc">Limit GTC</option>
              <option value="limit_ioc">Limit IOC</option>
              <option value="market_ioc">Market IOC</option>
            </select>
          </label>
          <label>
            Demo client
            <select value={client} onChange={(e) => setClient(e.target.value)}>
              <option value="1">Client 1</option>
              <option value="2">Client 2</option>
              <option value="3">Client 3</option>
            </select>
          </label>
        </div>
        <div className="form-pair">
          <label>
            Quantity
            <input
              inputMode="numeric"
              required
              value={quantity}
              onChange={(e) => setQuantity(e.target.value)}
              maxLength={20}
            />
          </label>
          {type !== 'market_ioc' && (
            <label>
              Limit price · ticks
              <input
                inputMode="numeric"
                required
                value={price}
                onChange={(e) => setPrice(e.target.value)}
                maxLength={20}
              />
            </label>
          )}
        </div>
        <p className="order-explainer">
          {type === 'limit_gtc'
            ? 'Trades at your limit or better. Unfilled quantity stays in the book.'
            : type === 'limit_ioc'
              ? 'Trades at your limit or better. Any unfilled quantity is canceled.'
              : 'Consumes available opposing liquidity. Any unfilled quantity is canceled; this order never rests.'}
        </p>
        <button className={`submit-order ${side}`} type="submit">
          Submit {side} order
        </button>
      </fieldset>
      {error && (
        <p role="alert" className="error-text">
          {error}
        </p>
      )}
      <small>Order IDs are assigned by the engine service.</small>
    </form>
  );
}

function Orders({
  view,
  disabled,
  setPage,
  mutate,
}: {
  view: View;
  disabled: boolean;
  setPage: (page: number) => void;
  mutate: Mutate;
}) {
  const [replacement, setReplacement] = useState<Order | null>(null);
  useEffect(() => setReplacement(null), [view.activation, view.instrument_id]);
  return (
    <>
      <div className="section-head">
        <div>
          <h2>
            Open orders <span className="count">{view.active_count}</span>
          </h2>
          <p>50 rows per page · Oldest priority first</p>
        </div>
        <div className="actions">
          <button
            aria-label="Previous orders page"
            disabled={view.orders_page <= 1}
            onClick={() => setPage(view.orders_page - 1)}
          >
            Previous
          </button>
          <span>
            {view.orders_page} / {view.orders_pages}
          </span>
          <button
            aria-label="Next orders page"
            disabled={view.orders_page >= view.orders_pages}
            onClick={() => setPage(view.orders_page + 1)}
          >
            Next
          </button>
        </div>
      </div>
      <div className="table-scroll" tabIndex={0} aria-label="Scrollable open orders">
        <table>
          <thead>
            <tr>
              {['Order ID', 'Client', 'Side', 'Price', 'Remaining', 'Priority', 'Actions'].map(
                (h) => (
                  <th key={h}>{h}</th>
                ),
              )}
            </tr>
          </thead>
          <tbody>
            {view.orders.map((o) => (
              <tr key={o.order_id}>
                <td>{format(o.order_id)}</td>
                <td>{o.client_id}</td>
                <td>
                  <span className={`side-badge ${o.side}`}>
                    {o.side === 'buy' ? 'Buy' : 'Sell'}
                  </span>
                </td>
                <td>{format(o.price)}</td>
                <td>{format(o.quantity)}</td>
                <td>{format(o.priority)}</td>
                <td>
                  <div className="actions">
                    <button
                      disabled={disabled}
                      aria-label={`Replace order ${o.order_id}`}
                      onClick={() => setReplacement(o)}
                    >
                      Replace
                    </button>
                    <button
                      disabled={disabled}
                      aria-label={`Cancel order ${o.order_id}`}
                      onClick={() =>
                        void mutate('/commands', {
                          kind: 'cancel',
                          instrument_id: o.instrument_id,
                          order_id: o.order_id,
                        })
                      }
                    >
                      Cancel
                    </button>
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {!view.orders.length && <p className="empty">No open orders for this instrument.</p>}
      {replacement && (
        <Modal title={`Replace order ${replacement.order_id}`} close={() => setReplacement(null)}>
          <ReplaceForm
            order={replacement}
            disabled={disabled}
            onReplace={async (body) => {
              if (await mutate('/commands', body)) setReplacement(null);
            }}
          />
        </Modal>
      )}
    </>
  );
}

function ReplaceForm({
  order,
  disabled,
  onReplace,
}: {
  order: Order;
  disabled: boolean;
  onReplace: (body: object) => void;
}) {
  const [price, setPrice] = useState(order.price);
  const [quantity, setQuantity] = useState(order.quantity);
  return (
    <form
      onSubmit={(e) => {
        e.preventDefault();
        onReplace({
          kind: 'replace',
          instrument_id: order.instrument_id,
          order_id: order.order_id,
          price,
          quantity,
        });
      }}
    >
      <p>
        Replacement creates a new Order ID and resets priority, even at the same price. It may trade
        immediately.
      </p>
      <p>
        Client {order.client_id} · {order.side} ·{' '}
        {order.instrument_id === '7' ? 'DEMO-A' : 'DEMO-B'}
      </p>
      <label>
        New limit price · ticks
        <input
          autoFocus
          value={price}
          onChange={(e) => setPrice(e.target.value)}
          pattern="-?(0|[1-9][0-9]*)"
          required
          maxLength={20}
        />
      </label>
      <label>
        New quantity
        <input
          value={quantity}
          onChange={(e) => setQuantity(e.target.value)}
          pattern="0|[1-9][0-9]*"
          required
          maxLength={20}
        />
      </label>
      <button disabled={disabled} className="primary">
        Replace and reset priority
      </button>
    </form>
  );
}

function Activity({ view, tab }: { view: View; tab: 'trades' | 'events' }) {
  return (
    <>
      <div className="section-head">
        <h2>{tab === 'trades' ? 'Recent trades' : 'Events'}</h2>
      </div>
      <p className="activity-marker">{view.activity_marker}</p>
      <p className="muted">
        Latest {tab === 'trades' ? '100 trades' : '200 events'} · Selected instrument
      </p>
      <div
        className="activity-feed"
        tabIndex={0}
        role="region"
        aria-label={tab === 'trades' ? 'Scrollable recent trades' : 'Scrollable events'}
      >
        {tab === 'trades'
          ? view.trades.map((t) => (
              <article className="trade" key={t.id}>
                <div>
                  <strong className={t.side}>
                    {t.side === 'buy' ? 'Buy' : 'Sell'} {format(t.quantity)} @ {format(t.price)}
                  </strong>
                  <span>Command {format(t.sequence)}</span>
                </div>
                <small>
                  Aggressor {t.aggressor_id} → Resting {t.resting_id}
                </small>
              </article>
            ))
          : view.events.map((e) => (
              <details
                key={`${view.activation}:${e.id}`}
                className={e.kind === 'Rejected' ? 'rejected-event' : ''}
              >
                <summary>
                  <span className="event-kind">{e.kind}</span>
                  <span>#{format(e.sequence)}</span>
                  <p>{e.description}</p>
                </summary>
                <h3>Command details</h3>
                <dl>
                  {Object.entries(e.command).map(([k, v]) => (
                    <div key={k}>
                      <dt>{k.replaceAll('_', ' ')}</dt>
                      <dd>{v}</dd>
                    </div>
                  ))}
                </dl>
                <h3>Event details</h3>
                <dl>
                  {Object.entries(e.details).map(([k, v]) => (
                    <div key={k}>
                      <dt>{k.replaceAll('_', ' ')}</dt>
                      <dd>{v}</dd>
                    </div>
                  ))}
                </dl>
              </details>
            ))}
        {!(tab === 'trades' ? view.trades : view.events).length && (
          <p className="empty">
            {tab === 'trades'
              ? 'No trades yet. Submit a crossing order or step through a scenario.'
              : 'No events since opening this session.'}
          </p>
        )}
      </div>
    </>
  );
}

function ScenarioControls({
  view,
  disabled,
  pending,
  mutate,
}: {
  view: View;
  disabled: boolean;
  pending: boolean;
  mutate: Mutate;
}) {
  const p = view.playback;
  const complete = p.cursor === p.total;
  return (
    <section className="scenario-strip" aria-label="Scenario playback">
      <div className="scenario-progress">
        <span className="eyebrow">{p.playing ? 'PLAYING' : complete ? 'COMPLETE' : 'PAUSED'}</span>
        <h2>{p.scenario_name}</h2>
        <progress value={p.cursor} max={p.total} aria-label="Scenario progress" />
        <span>
          Step {p.cursor} of {p.total}
        </span>
      </div>
      <p className="next-command">
        {p.next_description
          ? `Next: ${p.next_description}`
          : 'Scenario complete. Restart creates a new saved run.'}
      </p>
      <div className="playback-actions">
        <button
          className="primary"
          disabled={pending || (!p.playing && (disabled || complete))}
          onClick={() => void mutate('/playback', { action: p.playing ? 'pause' : 'play' })}
        >
          {p.playing ? 'Pause' : 'Play'}
        </button>
        <button
          disabled={disabled || p.playing || complete}
          onClick={() => void mutate('/playback', { action: 'step' })}
        >
          Step
        </button>
        <label>
          Speed
          <select
            disabled={pending}
            value={p.speed}
            onChange={(e) =>
              void mutate('/playback', { action: 'speed', speed: Number(e.target.value) })
            }
          >
            {[1, 2, 5, 10].map((s) => (
              <option key={s} value={s}>
                {s} cmd/s
              </option>
            ))}
          </select>
        </label>
        <button
          disabled={pending}
          onClick={() =>
            void mutate('/sessions', {
              name: `${p.scenario_name} · Restart`,
              mode: 'scenario',
              restart: true,
            })
          }
        >
          Restart scenario
        </button>
      </div>
    </section>
  );
}

function CreateForm({
  mode,
  scenarios,
  pending,
  onCreate,
}: {
  mode: 'manual' | 'scenario';
  scenarios: Scenario[];
  pending: boolean;
  onCreate: (body: object) => void;
}) {
  const [scenario, setScenario] = useState(scenarios[0]?.id ?? 'fifo');
  const [name, setName] = useState(mode === 'manual' ? 'Untitled sandbox' : 'Price-time priority');
  return (
    <form
      onSubmit={(e) => {
        e.preventDefault();
        onCreate({ name, mode, ...(mode === 'scenario' ? { scenario_id: scenario } : {}) });
      }}
    >
      {mode === 'scenario' && (
        <label>
          Scenario
          <select
            value={scenario}
            onChange={(e) => {
              setScenario(e.target.value);
              setName(scenarios.find((s) => s.id === e.target.value)?.name ?? 'Scenario');
            }}
          >
            {scenarios.map((s) => (
              <option key={s.id} value={s.id}>
                {s.name} · {s.step_count} steps
              </option>
            ))}
          </select>
        </label>
      )}
      {mode === 'scenario' && <p>{scenarios.find((s) => s.id === scenario)?.description}</p>}
      <label>
        Session name
        <input
          autoFocus
          value={name}
          onChange={(e) => setName(e.target.value)}
          required
          maxLength={80}
        />
      </label>
      <p>
        {mode === 'manual'
          ? 'Starts with two empty, independent books. Your existing session stays saved.'
          : 'Starts paused with an empty engine. Every book-building command is a visible step.'}
      </p>
      <button className="primary" disabled={pending}>
        Create {mode === 'manual' ? 'sandbox' : 'scenario'}
      </button>
    </form>
  );
}

function date(value: string) {
  return value ? new Date(value).toLocaleString() : 'Unknown';
}
