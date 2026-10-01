// Geometry is checked without a browser, network, font renderer, or DOM.
// Exact xterm measurements are supplied, including device-pixel rounding.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { mainScreenLayout } from './fixtures/main-screen-layout.mjs';

const directory = process.argv[2] || 'web';
const source = readFileSync(join(directory, 'vc-layout.js'), 'utf8');
const { screenLayout } = await import(`data:text/javascript,${encodeURIComponent(source)}`);
assert.equal(typeof screenLayout, 'function', 'screen sizing must be a pure exported function');

const ideal = size => ({ width: size * 40, height: size * 25 });
const rounded = dpr => size => ({ width: 80 * Math.max(1, Math.floor(size / 2 * dpr)) / dpr,
  height: 25 * Math.max(1, Math.floor(size * dpr)) / dpr });
// The vendored DOM renderer rounds the entire screen width and rounds the
// height after ceiling each device-pixel character height. The floor variants
// above deliberately stress conservative-estimate and fallback-font branches.
const xterm = dpr => size => ({ width: Math.round(size / 2 * dpr * 80 / dpr),
  height: Math.round(Math.ceil(size * dpr) * 25 / dpr) });
const variants = [['ideal', ideal, true],
  ...[1, 1.25, 1.5, 2].map(dpr => [`floor DPR ${dpr}`, rounded(dpr), false]),
  ...[1, 1.25, 1.5, 2].map(dpr => [`xterm DPR ${dpr}`, xterm(dpr), true])];
function resolve(input, metric = ideal) {
  const measurements = { [input.fontSize ?? 16]: Object.freeze(metric(input.fontSize ?? 16)) };
  for (let step = 0; step < 100; step++) {
    const result = screenLayout(Object.freeze({ ...input,
      footer: Object.freeze({ ...input.footer }),
      controls: input.controls ? Object.freeze({ ...input.controls }) : null,
      measurements: Object.freeze({ ...measurements }),
    }));
    if (!Object.hasOwn(result, 'measure')) return result;
    assert.ok(Number.isInteger(result.measure) && result.measure > 0, 'request a real font size');
    assert.equal(measurements[result.measure], undefined, 'never request a supplied measurement twice');
    measurements[result.measure] = Object.freeze(metric(result.measure));
  }
  assert.fail('font measurement requests did not converge');
}

let cases = 0, roundTrips = 0;
for (const [name, metric, stableAnchor] of variants) {
  for (let horizontal = 320; horizontal <= 2560; horizontal += 16) {
    for (let vertical = 480; vertical <= 1440; vertical += 16) {
      for (const [width, height] of [[horizontal, vertical], [vertical, horizontal]]) {
        for (const keys of [false, true]) {
          // Supply a realistic measured footer width/height. Wrapping is a
          // browser measurement, not a hidden approximation in the fitter.
          const footer = { width: Math.min(1050, width - 24),
            height: 18.5 + Math.ceil(1050 / (width - 24)) * 17.4 };
          const input = { width, height, footer, controls: keys ? { width: Math.min(640, width - 24), height: 244 } : null,
            safeCenter: keys && width < height, fontSize: 16, open: false };
          const closed = resolve(input, metric);
          assert.deepEqual(closed, mainScreenLayout(input, metric),
            `closed page differs from main at ${width}x${height}, keys=${keys}, ${name}`);
          if (stableAnchor) {
            const opened = resolve({ ...input, open: true }, metric);
            assert.deepEqual(resolve({ ...input, fontSize: opened.fontSize }, metric), closed,
              `open/close must restore ${width}x${height}, keys=${keys}, ${name}`);
            roundTrips++;
          }
          cases++;
        }
      }
    }
  }
}
console.log(`PASS closed layout: ${cases} exact main font/screen/footer/keypad comparisons, both orientations and nine font-metric variants`);
console.log(`PASS toggle restoration: ${roundTrips} full-grid round trips with ideal and actual xterm DPR metrics`);

for (const [, metric] of variants) for (const fontSize of [1, 9, 10, 15, 16, 17, 32, 48]) {
  for (const [width, height] of [[390, 480], [416, 844], [912, 600], [1440, 900], [2560, 1440]]) {
    const input = { width, height, footer: { width: Math.min(1050, width - 24), height: 70.7 }, fontSize };
    assert.deepEqual(resolve(input, metric), mainScreenLayout(input, metric),
      'session resize anchors must preserve every step of main\'s arithmetic');
  }
}

// Exercise main's two easily lost rounding branches explicitly: a next size
// can fit even when the estimate did not select it, or the estimate can fail.
for (const input of [
  { width: 424, height: 1000, footer: { width: 300, height: 40 }, fontSize: 16 },
  { width: 390, height: 844, footer: { width: 366, height: 88 }, fontSize: 9 },
]) assert.deepEqual(resolve(input, rounded(1)), mainScreenLayout(input, rounded(1)));
assert.equal(resolve({ width: 424, height: 1000, footer: { width: 300, height: 40 } }, rounded(1)).fontSize, 11,
  'device-pixel rounding can make main choose the next size');
assert.equal(resolve({ width: 390, height: 844, footer: { width: 366, height: 88 }, fontSize: 9 }, rounded(1)).fontSize, 9,
  'an overestimated size must be reduced before the next-size probe');

for (const width of [1440, 1600]) {
  const input = { width, height: 900, footer: { width: 1050, height: 53.3 }, controls: null, fontSize: 16 };
  const closed = resolve(input), opened = resolve({ ...input, open: true });
  assert.equal(opened.panel.side, 'right', `${width}px laptops need a useful side panel`);
  assert.ok(opened.panel.width >= 60 * 8 + 20 + 16, '60 source columns plus padding/border and a scrollbar');
  assert.ok(opened.screen.width >= 360 && opened.screen.height >= 225, 'VC stays at least phone size');
  assert.ok(opened.screen.width < closed.screen.width, 'VC shrinks to make room for Source');
  assert.equal(opened.screen.width / opened.screen.height, closed.screen.width / closed.screen.height);
  assert.ok(opened.fontSize < 16 || opened.fontSize % 16 === 0, 'larger fonts keep crisp native multiples');
  assert.ok(opened.screen.right + 12 <= opened.panel.left, 'the two panes cannot overlap');
  for (const pane of [opened.screen, opened.panel]) {
    assert.ok(pane.left >= 12 && pane.top >= 12 && pane.right <= width - 12 && pane.bottom <= 888,
      'both panes are completely visible');
  }
  assert.equal(opened.footer.left, closed.footer.left, 'the footer remains globally centered');
  assert.deepEqual(resolve({ ...input, open: false, fontSize: opened.fontSize }), closed, 'closing restores the exact main screen and footer');
}
console.log('PASS open laptop layout: 1440x900 and 1600x900 right-side, 60 columns, crisp shrink and exact close restoration');

const phoneInput = { width: 390, height: 844, footer: { width: 366, height: 88 },
  controls: { width: 366, height: 244 }, safeCenter: true };
const phoneClosed = resolve(phoneInput), phone = resolve({ ...phoneInput, open: true });
assert.equal(phone.panel.side, 'below');
assert.deepEqual(phone.screen, phoneClosed.screen, 'a phone keeps main\'s screen size and position');
assert.deepEqual(phone.controls, phoneClosed.controls, 'Source follows, rather than displacing, the keypad');
assert.equal(phone.panel.top, phone.controls.bottom + 12);
assert.equal(phone.panel.height, Math.max(196, 844 - 12 - phone.panel.top));
assert.ok(phone.panel.left >= 12 && phone.panel.right <= 378);
assert.equal(phone.footer.top, phone.panel.bottom + 12);
assert.deepEqual(resolve({ ...phoneInput, open: false }), phoneClosed);
for (const height of [480, 600, 900]) {
  const small = resolve({ ...phoneInput, height, open: true });
  assert.ok(small.panel.height >= 196, 'short phones still get ten lines and can scroll into view');
  assert.ok(small.panel.top >= small.controls.bottom + 12, 'the source panel never covers touch keys');
}
const below = resolve({ width: 800, height: 900, footer: { width: 760, height: 60 }, open: true });
assert.equal(below.panel.side, 'below');
assert.equal(below.panel.top, below.screen.bottom + 12, 'without touch keys the panel follows VC');
const threshold = resolve({ width: 912, height: 900, footer: { width: 800, height: 60 }, open: true });
assert.equal(threshold.panel.side, 'right', 'exactly enough room for a 360px screen and 60-column panel');
assert.equal(resolve({ width: 911, height: 900, footer: { width: 800, height: 60 }, open: true }).panel.side, 'below');
console.log('PASS open phone layout: 390x844 below keypad, viewport remainder, ten-line minimum, footer below and exact close restoration');

const scrollbarFailures = [];
const scrollbarCheck = (name, check) => {
  try { check(); console.log(`PASS ${name}`); }
  catch (error) { scrollbarFailures.push(`${name}: ${error.message}`); console.error(`FAIL ${name}: ${error.message}`); }
};
for (const [scrollbarSize, thresholdWidth] of [[17, 913], [0, 896], [24, 920]]) {
  scrollbarCheck(`native ${scrollbarSize}px scrollbar preserves 60 panel columns at its exact threshold`, () => {
    const input = { width: thresholdWidth, height: 900, footer: { width: 800, height: 60 }, open: true, scrollbarSize };
    assert.equal(resolve({ ...input, width: thresholdWidth - 1 }).panel.side, 'below');
    const atThreshold = resolve(input);
    assert.equal(atThreshold.panel.side, 'right');
    assert.equal(atThreshold.panel.width, 60 * 8 + 20 + scrollbarSize);
    assert.ok(atThreshold.screen.width >= 360);
  });
  scrollbarCheck(`native ${scrollbarSize}px horizontal scrollbar leaves ten source lines`, () => {
    const short = resolve({ ...phoneInput, height: 480, open: true, scrollbarSize });
    assert.equal(short.panel.height, 10 * 16 + 20 + scrollbarSize);
  });
}
assert.equal(scrollbarFailures.length, 0, scrollbarFailures.join('\n'));
