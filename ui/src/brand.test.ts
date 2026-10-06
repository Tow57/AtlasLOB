import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

const dark = readFileSync(new URL('../public/atlaslob-logo.svg', import.meta.url), 'utf8');
const light = readFileSync(new URL('../public/atlaslob-logo-light.svg', import.meta.url), 'utf8');

describe('Atlas brand assets', () => {
  it('ships genuine vector paths with a scalable viewBox and no embedded raster or grid', () => {
    for (const svg of [dark, light]) {
      expect(svg).toContain('viewBox="0 0 195 313"');
      expect(svg.match(/<path /g)).toHaveLength(7);
      expect(svg).not.toMatch(
        /<(image|rect|script|foreignObject)\b|data:image|https?:\/\/(?!www.w3.org)/,
      );
      expect(svg).toContain('Atlas supporting the globe');
    }
  });
  it('preserves identical geometry in the two theme variants', () => {
    expect(dark.replaceAll('#16181b', '#f4f6f8')).toBe(light);
  });
});
