import { describe, expect, it } from 'vitest';
import { barPercent, format, validInteger } from './exact';

describe('exact engine values', () => {
  it('formats the whole 64-bit range without rounding', () => {
    expect(format('9007199254740993')).toBe('9,007,199,254,740,993');
    expect(format('18446744073709551615')).toBe('18,446,744,073,709,551,615');
    expect(format('-9223372036854775808')).toBe('-9,223,372,036,854,775,808');
  });
  it('uses bounded numeric presentation ratios only', () => {
    expect(barPercent('9007199254740993', 18014398509481986n)).toBe(50);
    expect(barPercent('0', 0n)).toBe(0);
  });
  it('rejects floating-point, exponent and separator syntax', () => {
    for (const invalid of ['1.5', '1e3', '1,000', ' 3', '+2', '01', ''])
      expect(validInteger(invalid)).toBe(false);
    expect(validInteger('0')).toBe(true);
    expect(validInteger('-1', true)).toBe(true);
  });
});
