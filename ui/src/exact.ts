/** Never pass engine integers through Number, parseInt, or floating-point formatting. */
export function format(value: string | null | undefined): string {
  return value == null ? '—' : BigInt(value).toLocaleString('en-US');
}

export function barPercent(quantity: string, maximum: bigint): number {
  // Only the bounded 0..100 presentation ratio becomes a JavaScript number.
  return maximum === 0n ? 0 : Number((BigInt(quantity) * 100n) / maximum);
}

export function validInteger(value: string, signed = false): boolean {
  return (signed ? /^-?(0|[1-9][0-9]*)$/ : /^(0|[1-9][0-9]*)$/).test(value);
}
