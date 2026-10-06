import { useEffect, useRef, useState } from 'react';
import { request, type View, type ChartView, type Candle, type DepthPoint } from './api';
import { format } from './exact';
import { bounds, coordinate, sameChartSession, ticks } from './chartMath';

type Kind = 'price' | 'depth';
type Range = '100' | '500' | 'all';

export function Charts({
  view,
  createDemo,
  disabled,
}: {
  view: View;
  createDemo: () => void;
  disabled: boolean;
}) {
  const [kind, setKind] = useState<Kind>('price');
  const [range, setRange] = useState<Range>('500');
  const [chart, setChart] = useState<ChartView | null>(null);
  const [error, setError] = useState('');
  const [retry, setRetry] = useState(0);
  const latest = useRef(view);
  latest.current = view;
  useEffect(() => {
    let canceled = false;
    let inFlight = false;
    let failed = false;
    let acceptedRevision = -1;
    setError('');
    const poll = async () => {
      if (
        canceled ||
        inFlight ||
        failed ||
        document.visibilityState !== 'visible' ||
        acceptedRevision >= latest.current.chart_revision
      )
        return;
      inFlight = true;
      try {
        const next = await request<ChartView>(
          `/chart?instrument=${view.instrument_id}&kind=${kind}&range=${range}`,
        );
        // Coalesce revisions while a read is in flight. Accept useful progress even
        // when the book has advanced; never regress the displayed chart.
        if (
          !canceled &&
          sameChartSession(next, { ...latest.current, chart_revision: acceptedRevision })
        ) {
          acceptedRevision = next.revision;
          setChart(next);
        }
      } catch (e) {
        if (!canceled) {
          setError(String(e));
          failed = true;
        }
      } finally {
        inFlight = false;
      }
    };
    void poll();
    const timer = setInterval(() => void poll(), 150);
    return () => {
      canceled = true;
      clearInterval(timer);
    };
  }, [view.instance_id, view.activation, view.instrument_id, kind, range, retry]);
  const usable =
    chart &&
    chart.kind === kind &&
    chart.range === range &&
    chart.activation === view.activation &&
    chart.instance_id === view.instance_id &&
    chart.instrument_id === view.instrument_id;
  const summary = view.instruments.find((i) => i.instrument_id === view.instrument_id);
  return (
    <section className="panel chart-panel" aria-label="Market chart">
      <div className="section-head chart-heading">
        <div>
          <p className="eyebrow">{summary?.name ?? 'Instrument'} · INTEGER TICKS</p>
          <div className="last-price">
            <strong>{format(summary?.last_price)}</strong>
            {summary?.change != null && (
              <span className={BigInt(summary.change) < 0n ? 'sell' : 'buy'}>
                {BigInt(summary.change) > 0n ? '+' : ''}
                {format(summary.change)} ticks
              </span>
            )}
          </div>
          <p>Last execution · Change since first execution</p>
        </div>
        <div className="segmented" role="group" aria-label="Chart type">
          <button aria-pressed={kind === 'price'} onClick={() => setKind('price')}>
            Price
          </button>
          <button aria-pressed={kind === 'depth'} onClick={() => setKind('depth')}>
            Depth
          </button>
        </div>
      </div>
      <div className="chart-toolbar">
        <span>
          {kind === 'price' ? 'Executions & traded volume' : 'Full-book cumulative quantity'}
        </span>
        {kind === 'price' && (
          <div className="segmented" role="group" aria-label="Chart range">
            {(['100', '500', 'all'] as const).map((r) => (
              <button key={r} aria-pressed={range === r} onClick={() => setRange(r)}>
                {r === 'all' ? 'All' : `Last ${r}`}
              </button>
            ))}
          </div>
        )}
      </div>
      {error ? (
        <div className="empty" role="alert">
          <p>Chart unavailable. Order state is unchanged.</p>
          <p>{error}</p>
          <button onClick={() => setRetry(retry + 1)}>Retry chart</button>
        </div>
      ) : usable ? (
        kind === 'price' ? (
          <PriceChart data={chart} createDemo={createDemo} disabled={disabled} />
        ) : (
          <DepthChart data={chart} />
        )
      ) : (
        <p className="empty chart-loading" role="status">
          Loading verified chart data…
        </p>
      )}
      <div className="chart-caption">
        <span>
          {kind === 'price'
            ? `${usable ? chart.bucket_size : '—'} command(s) per candle · No calendar timestamps`
            : 'Current liquidity · Independent of visible ladder levels'}
        </span>
        <span>
          {usable &&
            `Through #${format(chart.sequence)}${chart.revision < view.chart_revision ? ' · Updating…' : ''}`}
        </span>
      </div>
    </section>
  );
}

function useWidth() {
  const ref = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(600);
  useEffect(() => {
    const node = ref.current;
    if (!node) return;
    const observer = new ResizeObserver(([entry]) =>
      setWidth(Math.max(260, entry.contentRect.width)),
    );
    observer.observe(node);
    return () => observer.disconnect();
  }, []);
  return { ref, width };
}

function candleDescription(candle: Candle) {
  return `Commands ${format(candle.start_sequence)}–${format(candle.end_sequence)}. ${candle.open === null ? 'No executions.' : `Open ${format(candle.open)}, high ${format(candle.high)}, low ${format(candle.low)}, close ${format(candle.close)} ticks.`} Volume ${format(candle.volume)}.`;
}

function PriceChart({
  data,
  createDemo,
  disabled,
}: {
  data: ChartView;
  createDemo: () => void;
  disabled: boolean;
}) {
  const { ref, width } = useWidth();
  const [selected, setSelected] = useState<number | null>(null);
  const candles = data.candles;
  const executions = candles.filter((c) => c.open !== null);
  const [min, max] = bounds(executions.flatMap((c) => [BigInt(c.low!), BigInt(c.high!)]));
  const axis = ticks(min, max);
  const right =
    width - Math.min(210, Math.max(72, ...axis.map((n) => format(String(n)).length * 7 + 15)));
  const left = 14,
    top = 12,
    bottom = 223,
    volumeTop = 254,
    volumeBottom = 305;
  const maximumVolume = candles.reduce((v, c) => (BigInt(c.volume) > v ? BigInt(c.volume) : v), 0n);
  const y = (value: string) => coordinate(BigInt(value), min, max, bottom, top);
  const unit = (right - left) / Math.max(1, candles.length);
  const active = candles.length ? Math.min(selected ?? candles.length - 1, candles.length - 1) : 0;
  const current = candles[active];
  return (
    <div ref={ref} className="chart-content">
      {!executions.length ? (
        <div className="empty chart-empty">
          <strong>No executions in this range</strong>
          <p>Resting orders build depth. Executed trades build the price chart.</p>
          <button disabled={disabled} onClick={createDemo}>
            Create Market Walk
          </button>
        </div>
      ) : (
        <div
          tabIndex={0}
          role="group"
          aria-label="Price chart; use left and right arrows to inspect candles"
          className="chart-focus"
          onKeyDown={(e) => {
            if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) return;
            e.preventDefault();
            setSelected(
              e.key === 'Home'
                ? 0
                : e.key === 'End'
                  ? candles.length - 1
                  : Math.max(
                      0,
                      Math.min(candles.length - 1, active + (e.key === 'ArrowRight' ? 1 : -1)),
                    ),
            );
          }}
        >
          <svg
            viewBox={`0 0 ${width} 336`}
            role="img"
            aria-label="Execution candlesticks and volume by command sequence"
            onPointerMove={(e) => {
              const x =
                ((e.clientX - e.currentTarget.getBoundingClientRect().left) * width) /
                e.currentTarget.getBoundingClientRect().width;
              setSelected(Math.max(0, Math.min(candles.length - 1, Math.floor((x - left) / unit))));
            }}
          >
            {axis.map((value) => (
              <g key={String(value)} className="chart-grid">
                <line x1={left} x2={right} y1={y(String(value))} y2={y(String(value))} />
                <text x={right + 8} y={y(String(value)) + 4}>
                  {format(String(value))}
                </text>
              </g>
            ))}
            <text x={left} y={246} className="chart-axis-label">
              Traded quantity
            </text>
            {candles.map((c, i) => {
              const x = left + unit * (i + 0.5),
                body = Math.max(0.6, Math.min(12, unit * 0.65));
              const cls =
                c.open !== null && BigInt(c.close!) < BigInt(c.open) ? 'candle-down' : 'candle-up';
              const volumeY = coordinate(
                BigInt(c.volume),
                0n,
                maximumVolume || 1n,
                volumeBottom,
                volumeTop,
              );
              return (
                <g key={c.start_sequence} className={cls}>
                  <title>{candleDescription(c)}</title>
                  {c.open !== null && (
                    <>
                      <line x1={x} x2={x} y1={y(c.high!)} y2={y(c.low!)} />
                      <rect
                        x={x - body / 2}
                        y={Math.min(y(c.open), y(c.close!))}
                        width={body}
                        height={Math.max(1, Math.abs(y(c.open) - y(c.close!)))}
                      />
                    </>
                  )}
                  <rect
                    className="volume-bar"
                    x={x - body / 2}
                    y={volumeY}
                    width={body}
                    height={volumeBottom - volumeY}
                  />
                </g>
              );
            })}
            {current && (
              <line
                className="crosshair"
                x1={left + unit * (active + 0.5)}
                x2={left + unit * (active + 0.5)}
                y1={top}
                y2={volumeBottom}
              />
            )}
            <g className="chart-axis-label">
              <text x={left} y={330}>
                #{format(data.start_sequence)}
              </text>
              <text x={right} y={330} textAnchor="end">
                #{format(data.end_sequence)}
              </text>
              <text x={(left + right) / 2} y={330} textAnchor="middle">
                Command sequence
              </text>
            </g>
          </svg>
        </div>
      )}
      {current && (
        <div className="chart-inspector" aria-live="polite" aria-atomic="true">
          {candleDescription(current)}
        </div>
      )}
    </div>
  );
}

function DepthChart({ data }: { data: ChartView }) {
  const { ref, width } = useWidth();
  const [selected, setSelected] = useState(0);
  const points = [
    ...data.bids.map((p) => ({ ...p, side: 'Bid' })),
    ...data.asks.map((p) => ({ ...p, side: 'Ask' })),
  ].sort((a, b) =>
    BigInt(a.price) < BigInt(b.price) ? -1 : BigInt(a.price) > BigInt(b.price) ? 1 : 0,
  );
  const [min, max] = bounds(points.map((p) => BigInt(p.price)));
  const maximum = points.reduce(
    (v, p) => (BigInt(p.cumulative) > v ? BigInt(p.cumulative) : v),
    1n,
  );
  const left = 14,
    right = width - 22,
    top = 20,
    bottom = 263;
  const x = (price: string) => coordinate(BigInt(price), min, max, left, right);
  const y = (q: string) => coordinate(BigInt(q), 0n, maximum, bottom, top);
  const path = (levels: DepthPoint[]) =>
    levels
      .map((p, i) =>
        i ? `H${x(p.price)}V${y(p.cumulative)}` : `M${x(p.price)},${bottom}V${y(p.cumulative)}`,
      )
      .join(' ');
  const active = points[Math.min(selected, points.length - 1)];
  return (
    <div ref={ref} className="chart-content">
      <div className="depth-legend">
        <span className="buy">Bid depth</span>
        <span className="sell">Ask depth</span>
        <span>Max quantity {format(String(maximum))}</span>
      </div>
      {!points.length ? (
        <div className="empty chart-empty">
          <strong>No resting liquidity</strong>
          <p>Submit a limit order or step through a scenario to build the book.</p>
        </div>
      ) : (
        <div
          tabIndex={0}
          role="group"
          aria-label="Depth chart; use left and right arrows to inspect levels"
          className="chart-focus"
          onKeyDown={(e) => {
            if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) return;
            e.preventDefault();
            setSelected(
              e.key === 'Home'
                ? 0
                : e.key === 'End'
                  ? points.length - 1
                  : Math.max(
                      0,
                      Math.min(points.length - 1, selected + (e.key === 'ArrowRight' ? 1 : -1)),
                    ),
            );
          }}
        >
          <svg
            viewBox={`0 0 ${width} 336`}
            role="img"
            aria-label="Cumulative bid and ask depth across the full book"
            onPointerMove={(e) => {
              const px =
                ((e.clientX - e.currentTarget.getBoundingClientRect().left) * width) /
                e.currentTarget.getBoundingClientRect().width;
              let nearest = 0;
              for (let i = 1; i < points.length; i++)
                if (Math.abs(x(points[i].price) - px) < Math.abs(x(points[nearest].price) - px))
                  nearest = i;
              setSelected(nearest);
            }}
          >
            {[0, 1, 2, 3, 4].map((i) => (
              <line
                className="depth-grid"
                key={i}
                x1={left}
                x2={right}
                y1={top + ((bottom - top) * i) / 4}
                y2={top + ((bottom - top) * i) / 4}
              />
            ))}
            {(
              [
                ['buy', data.bids],
                ['sell', data.asks],
              ] as const
            ).map(
              ([side, levels]) =>
                levels.length > 0 && (
                  <g key={side} className={`depth-${side}`}>
                    <path
                      className="depth-area"
                      d={`${path(levels)} L${x(levels[levels.length - 1].price)},${bottom} H${x(levels[0].price)} Z`}
                    />
                    <path className="depth-line" d={path(levels)} />
                    <circle
                      className="depth-level-marker"
                      cx={x(levels[0].price)}
                      cy={y(levels[0].cumulative)}
                      r={3}
                    />
                  </g>
                ),
            )}
            {active && (
              <circle
                className="depth-point"
                cx={x(active.price)}
                cy={y(active.cumulative)}
                r={4}
              />
            )}
            <g className="chart-axis-label">
              <text x={left} y={287}>
                {format(String(min))}
              </text>
              <text x={right} y={287} textAnchor="end">
                {format(String(max))}
              </text>
              <text x={(left + right) / 2} y={314} textAnchor="middle">
                Price · ticks
              </text>
            </g>
          </svg>
        </div>
      )}
      <div className="chart-inspector" aria-live="polite" aria-atomic="true">
        {active
          ? `${active.side} at ${format(active.price)} ticks · Level quantity ${format(active.quantity)} · Cumulative ${format(active.cumulative)}`
          : 'No levels to inspect.'}
      </div>
      {points.length > 0 && (!data.bids.length || !data.asks.length) && (
        <p className="muted one-sided">
          One-sided book · No {data.bids.length ? 'asks' : 'bids'} resting
        </p>
      )}
    </div>
  );
}
