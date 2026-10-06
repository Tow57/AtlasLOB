/** Convert only bounded drawing coordinates to floating point, never engine values. */
export function coordinate(value: bigint, min: bigint, max: bigint, start: number, end: number) {
  if (max <= min) return (start + end) / 2;
  const ratio = Number(((value - min) * 1_000_000n) / (max - min)) / 1_000_000;
  return start + ratio * (end - start);
}

export function bounds(values: bigint[]): [bigint, bigint] {
  if (!values.length) return [0n, 1n];
  let min = values[0],
    max = values[0];
  for (const value of values) {
    if (value < min) min = value;
    if (value > max) max = value;
  }
  const padding = (max - min) / 12n || 1n;
  return [min - padding, max + padding];
}

export function ticks(min: bigint, max: bigint): bigint[] {
  return [
    ...new Set(Array.from({ length: 5 }, (_, index) => min + ((max - min) * BigInt(index)) / 4n)),
  ];
}

export function sameChartSession(
  chart: {
    instance_id: string;
    activation: string | null;
    instrument_id: string;
    revision: number;
  },
  view: {
    instance_id: string;
    activation: string | null;
    instrument_id: string;
    chart_revision: number;
  },
) {
  return (
    chart.instance_id === view.instance_id &&
    chart.activation === view.activation &&
    chart.instrument_id === view.instrument_id &&
    chart.revision >= view.chart_revision
  );
}
