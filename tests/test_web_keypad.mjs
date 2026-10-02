import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';

// Check the published CSS, not just its source: WEB_OUT may be overridden.
const output = process.argv[2] || 'build/web';
const page = await readFile(join(output, 'index.html'), 'utf8');
assert.match(page, /id="keypad"[^>]*hidden/, 'the portrait keypad starts hidden');
const keypadMarkup = page.match(/<div\b[^>]*id="keypad"[^>]*>(.*?)<\/div>/s)?.[1];
assert.ok(keypadMarkup, 'inspect the real keypad markup');
const keypadButtons = [...keypadMarkup.matchAll(/<button\b([^>]*)>(.*?)<\/button>/gs)]
  .map(([, attributes, label]) => ({
    attributes: Object.fromEntries([...attributes.matchAll(/([\w-]+)="([^"]*)"/g)]
      .map(([, name, value]) => [name, value])),
    label: label.trim(),
  }));
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
  for (let flags = 0; flags < 8; flags++)
    assert.equal(keySequence(key, flags), report(code, flags), `${key}/${flags}`);
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
const buttons = keypadButtons.map(({ attributes }) => ({
  dataset: { key: attributes['data-key'] }, attributes: { ...attributes },
  setAttribute(name, value) { this.attributes[name] = value; },
  closest() { return this; },
}));
const buttonFor = key => buttons.find(button => button.dataset.key === key);
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
assert.equal(buttonFor('Control').attributes['aria-pressed'], 'true');
assert.equal(buttonFor('Alt').attributes['aria-pressed'], 'false');
assert.equal(buttonFor('Shift').attributes['aria-pressed'], 'true');
let prevented = 0;
for (const button of buttons)
  listeners.get('pointerdown')({ target: button, preventDefault: () => prevented++ });
assert.equal(prevented, buttons.length, 'all keypad taps keep focus and cannot scroll it into view');
assert.equal(focused, 0, 'even Keyboard waits for its click before opening the phone keyboard');
assert.deepEqual(pressed, [], 'pointerdown itself sends no key bytes');
for (const button of buttons) listeners.get('click')({ target: button, preventDefault() {} });
assert.deepEqual(pressed, buttons.filter(button => button.dataset.key !== 'Keyboard')
  .map(button => button.dataset.key), 'every published button routes its unchanged data-key');
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

// A long press anywhere on a touch screen must not select text or open the
// copy bubble, which misfires while tapping VC's panels (Eugene, 2026-10-02).
const coarseBlocks = [...page.matchAll(/@media\s*\(pointer:\s*coarse\)\s*\{([\s\S]*?)\n    \}/g)].map(m => m[1]);
const pageRule = coarseBlocks.map(b => b.match(/html,\s*body[^{]*\{([^}]*)\}/)?.[1]).find(Boolean) || '';
assert.match(pageRule, /-webkit-touch-callout:\s*none\s*;/, 'touch pages suppress the long-press callout');
assert.match(pageRule, /-webkit-user-select:\s*none\s*;/, 'touch pages suppress long-press text selection');
assert.match(pageRule, /(?:^|;)\s*user-select:\s*none\s*;/);
const webScript = await readFile(join(output, 'vc-web.js'), 'utf8');
assert.match(webScript, /addEventListener\("contextmenu"[^;]*coarse/s,
  'a long press on a touch screen never opens the context menu');

const expectedRows = [
  ['F1', 'F2', 'F3', 'F4', 'F5'],
  ['F6', 'F7', 'F8', 'F9', 'F10'],
  ['Escape', 'Tab', 'ArrowUp', 'Insert', 'Enter'],
  ['Control', 'ArrowLeft', 'ArrowDown', 'ArrowRight', 'Enter'],
  ['Shift', 'Alt', 'Keyboard', 'Keyboard', 'Keyboard'],
];
assert.deepEqual(keypadButtons.map(button => button.attributes['data-key']),
  [...new Set(expectedRows.flat())], 'five-row keypad order, including Shift then Alt in row five');
for (const { attributes, label } of keypadButtons) {
  assert.equal(attributes.type, 'button');
  if (/^F\d+$/.test(attributes['data-key']))
    assert.equal(label, attributes['data-key'], 'F-key labels contain only F and their number');
}
assert.match(keypadRule, /grid-template-columns:\s*repeat\(5,\s*minmax\(0,\s*1fr\)\)\s*;/);
assert.match(keypadRule, /grid-template-rows:\s*repeat\(5,\s*minmax\(44px,\s*auto\)\)\s*;/,
  'all five rows, including the two under Enter, stay at least 44px high');
const classRule = className => page.match(new RegExp(`#keypad \\.${className}\\s*\\{([^}]*)\\}`, 's'))?.[1] || '';
const enterRule = classRule('enter');
assert.match(enterRule, /grid-column:\s*5\s*;/, 'Enter occupies column five');
assert.match(enterRule, /grid-row:\s*3\s*\/\s*span\s+2\s*;/, 'Enter spans rows three and four');
assert.match(classRule('keyboard'), /grid-column:\s*span\s+3\s*;/);
assert.ok(buttonFor('Keyboard').attributes.class?.split(/\s+/).includes('keyboard'),
  'the published Keyboard button actually receives its three-column span');
assert.doesNotMatch(keypadMarkup, /class="[^"]*\bwide\b/, 'Shift no longer takes two columns');

// Model ordinary row-wise grid auto-placement, reserving Enter's explicit
// two-row box first. This catches order/span combinations that make six rows.
const grid = Array.from({ length: 5 }, () => Array(5).fill(null));
grid[2][4] = grid[3][4] = 'Enter';
let cursor = 0;
for (const { attributes } of keypadButtons) {
  const key = attributes['data-key'];
  if (key === 'Enter') continue;
  const span = key === 'Keyboard' ? 3 : 1;
  let placed = false;
  for (; cursor < 25; cursor++) {
    const row = Math.floor(cursor / 5), column = cursor % 5;
    if (column + span <= 5 && grid[row].slice(column, column + span).every(value => value === null)) {
      grid[row].fill(key, column, column + span);
      cursor += span;
      placed = true;
      break;
    }
  }
  assert.ok(placed, `${key} fits the five-row grid`);
}
assert.deepEqual(grid, expectedRows);

for (const [key, className, background, border, text] of [
  ['Escape', 'escape', '#5a1616', '#e24b4a', '#ffd6d6'],
  ['Tab', 'tab', '#4a3306', '#ba7517', '#ffe2b0'],
  ['Enter', 'enter', '#173d0a', '#639922', '#d8f0b8'],
]) {
  assert.ok(buttonFor(key).attributes.class?.split(/\s+/).includes(className), `${key} colour class`);
  const rule = classRule(className);
  assert.match(rule, new RegExp(`(?:^|;)\\s*background:\\s*${background}\\s*;`));
  assert.match(rule, new RegExp(`border-color:\\s*${border}\\s*;`));
  assert.match(rule, new RegExp(`(?:^|;)\\s*color:\\s*${text}\\s*;`));
}
for (const key of ['Control', 'Alt', 'Shift']) {
  assert.ok(buttonFor(key).attributes.class?.split(/\s+/).includes('modifier'), `${key} violet class`);
  assert.equal(keypadButtons.find(button => button.attributes['data-key'] === key)
    .attributes['aria-pressed'], 'false', `${key} starts unpressed`);
}
assert.match(classRule('modifier'), /border-color:\s*#7f77dd\s*;/i);
assert.match(classRule('modifier'), /(?:^|;)\s*color:\s*#cecbf6\s*;/i);
const stickyRule = page.match(/#keypad \.modifier\[aria-pressed="true"\]\s*\{([^}]*)\}/s)?.[1] || '';
assert.match(stickyRule, /(?:^|;)\s*background:\s*#534ab7\s*;/i);
assert.match(stickyRule, /(?:^|;)\s*color:\s*(?:#fff(?:fff)?|white)\s*;/i);

assert.match(page, /<meta\s+name="viewport"\s+content="width=device-width, initial-scale=1"\s*>/,
  'desktop and landscape start with the exact original viewport policy');
const portraitCSS = page.match(/@media\s*\(pointer: coarse\) and \(orientation: portrait\)\s*\{((?:[^{}]|\{[^{}]*\})*)\}/s)?.[1];
assert.ok(portraitCSS, 'only coarse-pointer portrait receives the page layout overrides');
assert.match(portraitCSS, /html, body\s*\{[^}]*overflow:\s*hidden\s*;/s);
const mainRule = portraitCSS.match(/main\s*\{([^}]*)\}/s)?.[1] || '';
assert.match(mainRule, /position:\s*fixed\s*;/);
assert.match(mainRule, /overflow:\s*hidden\s*;/);
assert.match(mainRule, /top:\s*var\(--vc-viewport-top,\s*0px\)\s*;/);
assert.match(mainRule, /height:\s*var\(--vc-viewport-height,\s*100%\)\s*;/);
assert.match(mainRule, /justify-content:\s*flex-start\s*;/);
assert.match(mainRule, /gap:\s*0\s*;/, 'the footer follows the screen without the old flex gap');
assert.match(mainRule, /padding:\s*env\(safe-area-inset-top\)\s+env\(safe-area-inset-right\)\s+env\(safe-area-inset-bottom\)\s+env\(safe-area-inset-left\)\s*;/,
  'only safe-area insets, never arbitrary screen padding');
const pinnedRule = portraitCSS.match(/#keypad\s*\{([^}]*)\}/s)?.[1] || '';
assert.match(pinnedRule, /position:\s*absolute\s*;/);
assert.match(pinnedRule, /bottom:\s*calc\(env\(safe-area-inset-bottom\)\s*\+\s*12px\)\s*;/,
  'the pinned pad keeps a 12px gap above the address bar');
assert.match(pinnedRule, /left:\s*env\(safe-area-inset-left\)\s*;/);
assert.match(pinnedRule, /right:\s*env\(safe-area-inset-right\)\s*;/);
assert.match(portraitCSS, /#keypad\[data-keyboard-open="true"\]\s*\{\s*display:\s*none\s*;\s*\}/,
  'phone keyboard hides the pad without changing its media-query-owned hidden state');
assert.ok(portraitCSS.indexOf('#keypad[data-keyboard-open="true"]')
  > portraitCSS.indexOf('#keypad:not([hidden])'), 'keyboard visibility wins over the normal portrait rule');
assert.match(portraitCSS, /#terminal\s*\{[^}]*overflow:\s*hidden\s*;/s);
assert.match(portraitCSS, /#terminal \.xterm\s*\{[^}]*transform-origin:\s*top left\s*;/s);
assert.match(portraitCSS, /#source-button\s*\{[^}]*position:\s*static\s*;[^}]*min-height:\s*44px\s*;[^}]*margin-top:\s*6px\s*;/s,
  'the portrait-only Source button participates in its footer without overlapping keys');
assert.ok(page.indexOf('id="terminal"') < page.indexOf('id="keypad"'));
const app = await readFile(new URL('../web/vc-web.js', import.meta.url), 'utf8');
assert.match(app, /portraitTouch\s*\?\s*`\$\{defaultViewportContent\}, viewport-fit=cover`\s*:\s*defaultViewportContent/,
  'only portrait touch opts into cover; leaving it restores the original viewport');
const fit = app.slice(app.indexOf('function fit('), app.indexOf('function onExit()'));
assert.ok(fit.includes('function fit('), 'inspect the actual fitting adapter');
assert.match(app, /terminal\.onData\(enqueue\)/, 'phone text keeps the existing input queue');
assert.match(app, /pointerdown.*terminal\?\.focus/, 'screen taps keep their mouse path');
assert.match(app, /terminal\?\.textarea\?\.focus\(\{\s*preventScroll:\s*true\s*\}\)/);
console.log('web keypad: five-row order/spans, colour classes, safe portrait CSS, all key bytes, sticky/physical/phone input and focus passed');
