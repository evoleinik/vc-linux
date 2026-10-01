import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';

// Check the published CSS, not just its source: WEB_OUT may be overridden.
const output = process.argv[2] || 'build/web';
const page = await readFile(join(output, 'index.html'), 'utf8');
assert.match(page, /id="keypad"[^>]*hidden/, 'the portrait keypad starts hidden');
const source = await readFile(new URL('../web/vc-keypad.js', import.meta.url), 'utf8');
const { PORTRAIT_QUERY, initialInput, reduceInput, keySequence, bindKeypad } =
  await import(`data:text/javascript,${encodeURIComponent(source)}`);
const report = (code, flags = 0, type) => `\x1b[${code};${flags + 1}${type ? `:${type}` : ''}u`;
const keyCodes = {
  F1: 57364, F2: 57365, F3: 57366, F4: 57367, F5: 57368,
  F6: 57369, F7: 57370, F8: 57371, F9: 57372, F10: 57373,
  ArrowLeft: 57350, ArrowRight: 57351, ArrowUp: 57352, ArrowDown: 57353,
  Escape: 27, Tab: 9, Insert: 57348, Enter: 13,
};
for (const [key, code] of Object.entries(keyCodes)) {
  assert.equal(keySequence(key), report(code), key);
  for (const flags of [1, 2, 4, 7]) assert.equal(keySequence(key, flags), report(code, flags), `${key}/${flags}`);
  const result = reduceInput(initialInput(), { type: 'pad', key });
  assert.equal(result.bytes, report(code), `${key} goes through the shared input reducer`);
  assert.equal(result.handled, true);
  assert.match(page, new RegExp(`data-key="${key}"`));
}
assert.equal(keySequence('Unidentified'), null, 'IME placeholders are not keys');
assert.equal(keySequence('ж', 1), report('ж'.codePointAt(0), 1));

let state = initialInput();
const frozen = structuredClone(state);
let result = reduceInput(state, { type: 'toggle', key: 'Control' });
assert.deepEqual(state, frozen, 'state transitions are pure');
assert.equal(result.state.sticky, 4);
assert.equal(result.bytes, report(57442, 4, 1), 'sticky Ctrl is held in VC before another key');
state = result.state;
result = reduceInput(state, { type: 'toggle', key: 'Control' });
assert.equal(result.state.sticky, 0);
assert.equal(result.bytes, report(57442, 0, 3), 'a second tap cancels the modifier');
result = reduceInput(state, { type: 'pad', key: 'F5' });
assert.equal(result.bytes, report(57368, 4) + report(57442, 0, 3));
assert.equal(result.state.sticky, 0, 'the next key consumes sticky Ctrl');
assert.equal(reduceInput(result.state, { type: 'pad', key: 'F5' }).bytes, report(57368));

for (const [key, flag, code] of [['Shift', 1, 57441], ['Alt', 2, 57443], ['Control', 4, 57442]]) {
  const down = reduceInput(initialInput(), { type: 'toggle', key });
  assert.equal(down.bytes, report(code, flag, 1));
  const up = reduceInput(down.state, { type: 'toggle', key });
  assert.equal(up.bytes, report(code, 0, 3));
  assert.equal(up.state.sticky, 0);
}
state = initialInput();
for (const key of ['Shift', 'Alt', 'Control']) state = reduceInput(state, { type: 'toggle', key }).state;
result = reduceInput(state, { type: 'pad', key: 'ArrowUp' });
assert.equal(result.bytes, report(57352, 7) + report(57441, 0, 3)
  + report(57443, 0, 3) + report(57442, 0, 3));
assert.equal(result.state.sticky, 0, 'combined modifiers are one-shot too');

const event = (key, extra = {}) => ({ type: 'keydown', key, code: '', ...extra });
state = reduceInput(initialInput(), { type: 'toggle', key: 'Control' }).state;
result = reduceInput(state, { type: 'key', event: event('m', { code: 'KeyM' }) });
assert.equal(result.bytes, report(109, 4) + report(57442, 0, 3), 'real keys consume sticky modifiers');
assert.equal(result.handled, true, 'xterm must not send a duplicate unmodified key');
assert.equal(reduceInput(result.state, { type: 'key', event: event('m') }).handled, false);
result = reduceInput(state, { type: 'key', event: event('Unidentified', { isComposing: true }) });
assert.equal(result.state.sticky, 4, 'composition waits for committed text');
assert.equal(result.handled, false);
result = reduceInput(state, { type: 'text', data: 'mhello' });
assert.equal(result.bytes, report(109, 4) + report(57442, 0, 3) + 'hello', 'phone input modifies only its first character');
assert.equal(result.state.sticky, 0);
for (const data of ['', '\x1b[I']) {
  result = reduceInput(state, { type: 'text', data });
  assert.equal(result.bytes, data, 'terminal reports retain their input bytes');
  assert.equal(result.state.sticky, 4);
}
for (const data of ['\x1b[<0;2;3M', '\x1b[<0;2;3m']) {
  result = reduceInput(state, { type: 'text', data });
  assert.equal(result.bytes, data + report(57442, 4, 1),
    'mouse bytes stay intact, then the held report restores VC\'s sticky key bar');
  assert.equal(result.state.sticky, 4, 'mouse clicks do not consume the next-key modifier');
}

state = reduceInput(state, { type: 'key', event: event('Shift', { code: 'ShiftRight', shiftKey: true }) }).state;
result = reduceInput(state, { type: 'pad', key: 'F5' });
assert.equal(result.bytes, report(57368, 5) + report(57442, 1, 3), 'consuming sticky Ctrl preserves physical Shift');
assert.equal(result.state.physical, 1);
result = reduceInput(result.state, { type: 'reset' });
assert.match(result.bytes, /\x1b\[57447;1:3u/, 'blur releases the actual physical side');
assert.deepEqual(result.state, initialInput());
state = reduceInput(initialInput(), { type: 'key', event: event('Control', { code: 'ControlLeft', ctrlKey: true }) }).state;
state = reduceInput(state, { type: 'toggle', key: 'Control' }).state;
result = reduceInput(state, { type: 'pad', key: 'F5' });
assert.equal(result.bytes, report(57368, 4), 'one-shot release must not release a physically held Ctrl');
result = reduceInput(result.state, { type: 'key', event: event('Control', { type: 'keyup', code: 'ControlLeft' }) });
assert.equal(result.bytes, report(57442, 0, 3));

for (const [code, key] of [[91, '['], [105, 'i'], [109, 'm'], [104, 'h']]) {
  result = reduceInput(initialInput(), { type: 'key', event: event(key, { ctrlKey: true }) });
  assert.equal(result.bytes, report(code, 4), `desktop Ctrl-${key} retains its scan code`);
}
for (const extra of [{ code: 'Pause', key: 'Pause' }, { code: 'KeyB', key: 'B', shiftKey: true }]) {
  result = reduceInput(initialInput(), { type: 'key', event: event(extra.key, { ctrlKey: true, ...extra }) });
  assert.equal(result.bytes, report(57362, 4), 'existing BASIC break shortcuts are unchanged');
}
result = reduceInput(initialInput(), { type: 'key', event: event('F3') });
assert.equal(result.handled, false, 'unmodified desktop F keys still use xterm');
assert.equal(result.preventDefault, true, 'the browser must not steal an F key');

// Minimal DOM doubles exercise the actual binding without a browser or a network.
const listeners = new Map();
const mediaListeners = new Map();
const media = {
  matches: false,
  addEventListener: (name, callback) => mediaListeners.set(name, callback),
  removeEventListener: (name) => mediaListeners.delete(name),
};
const buttons = ['Control', 'Alt', 'Shift', 'F1', 'Keyboard'].map(key => ({
  dataset: { key }, attributes: {},
  setAttribute(name, value) { this.attributes[name] = value; },
  closest() { return this; },
}));
const element = {
  hidden: true,
  querySelectorAll: () => buttons,
  contains: button => buttons.includes(button),
  addEventListener: (name, callback) => listeners.set(name, callback),
  removeEventListener: (name) => listeners.delete(name),
};
const pressed = [];
let focused = 0;
let cleared = 0;
const binding = bindKeypad(element, {
  matchMedia(query) {
    assert.equal(query, '(pointer: coarse) and (orientation: portrait)');
    assert.equal(query, PORTRAIT_QUERY);
    return media;
  },
  onKey: key => pressed.push(key),
  onKeyboard: () => focused++,
  onHide: () => cleared++,
});
assert.equal(element.hidden, true, 'desktop or landscape never gets a keypad');
media.matches = true;
mediaListeners.get('change')();
assert.equal(element.hidden, false, 'coarse-pointer portrait shows the keypad');
binding.setSticky(5);
assert.equal(buttons[0].attributes['aria-pressed'], 'true');
assert.equal(buttons[1].attributes['aria-pressed'], 'false');
assert.equal(buttons[2].attributes['aria-pressed'], 'true');
let prevented = 0;
listeners.get('pointerdown')({ target: buttons[3], preventDefault: () => prevented++ });
assert.equal(prevented, 1, 'keypad taps keep textarea focus and cannot scroll it into view');
listeners.get('click')({ target: buttons[3], preventDefault() {} });
assert.deepEqual(pressed, ['F1']);
listeners.get('click')({ target: buttons[4], preventDefault() {} });
assert.equal(focused, 1, 'the keyboard button focuses xterm synchronously');
media.matches = false;
mediaListeners.get('change')();
assert.equal(element.hidden, true, 'rotating or changing pointer type hides it again');
assert.ok(cleared >= 1, 'hiding the keypad releases sticky modifiers');
binding.dispose();
assert.equal(listeners.size, 0);
assert.equal(mediaListeners.size, 0);

const keypadRule = page.match(/#keypad\s*\{([^}]*)\}/s)?.[1] || '';
const buttonRule = page.match(/#keypad button\s*\{([^}]*)\}/s)?.[1] || '';
assert.match(keypadRule, /\btouch-action:\s*manipulation\s*;/,
  'the whole keypad, including gaps, prevents double-tap zoom');
assert.doesNotMatch(buttonRule, /\btouch-action\s*:/,
  'touch-action belongs to the keypad container, not just its buttons');
assert.match(buttonRule, /\bmin-height:\s*44px\s*;/);
assert.match(buttonRule, /(?:^|;)\s*user-select:\s*none\s*;/);
assert.match(buttonRule, /-webkit-user-select:\s*none\s*;/,
  'iOS buttons must not select text on a long press');
assert.match(buttonRule, /-webkit-touch-callout:\s*none\s*;/,
  'iOS buttons must not open the long-press callout');
assert.match(page, /@media\s*\(pointer: coarse\) and \(orientation: portrait\)/);
assert.ok(page.indexOf('id="terminal"') < page.indexOf('id="keypad"'));
const app = await readFile(new URL('../web/vc-web.js', import.meta.url), 'utf8');
const fit = app.slice(app.indexOf('function fit('), app.indexOf('function onExit()'));
assert.ok(fit.includes('function fit('), 'inspect the actual fitting adapter');
assert.doesNotMatch(fit, /keypad/i, 'the original screen fit never subtracts keypad height');
assert.match(app, /terminal\.onData\(enqueue\)/, 'phone text keeps the existing input queue');
assert.match(app, /pointerdown.*terminal\?\.focus/, 'screen taps keep their mouse path');
assert.match(app, /terminal\?\.textarea\?\.focus\(\{\s*preventScroll:\s*true\s*\}\)/);
console.log('web keypad: built-page iOS CSS, all key bytes, sticky/physical/phone input, media query, focus and unchanged screen fit passed');
