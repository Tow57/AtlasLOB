import { describe, expect, it } from 'vitest';
import { bounds, coordinate, sameChartSession, ticks } from './chartMath';

describe('exact chart drawing', () => {
  it('preserves adjacent ticks above the safe integer limit', () => {
    const start = 9007199254740992n;
    expect(coordinate(start + 1n, start, start + 2n, 0, 100)).toBe(50);
    expect(coordinate(start + 2n, start, start + 2n, 0, 100)).toBe(100);
  });
  it('normalizes full signed prices and arbitrary-size volumes', () => {
    expect(coordinate(0n, -9223372036854775808n, 9223372036854775807n, 0, 200)).toBeCloseTo(100);
    expect(coordinate(10n ** 30n, 0n, 2n * 10n ** 30n, 0, 100)).toBe(50);
    expect(bounds([5n, 5n])).toEqual([4n, 6n]);
    expect(ticks(4n, 6n)).toEqual([4n, 5n, 6n]);
  });
  it('rejects stale session, instance, instrument and revision responses', () => {
    const view = { instance_id: 'server', activation: 'a', instrument_id: '7', chart_revision: 2 };
    const response = { instance_id: 'server', activation: 'a', instrument_id: '7', revision: 2 };
    expect(sameChartSession(response, view)).toBe(true);
    for (const patch of [
      { activation: 'old' },
      { instance_id: 'old' },
      { instrument_id: '42' },
      { revision: 1 },
    ])
      expect(sameChartSession({ ...response, ...patch }, view)).toBe(false);
  });
});
