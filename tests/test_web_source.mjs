// Pure map/stack logic, real panel rendering, and page control wiring.
// All assets come from local files or tiny in-memory responses: no browser.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { runInNewContext } from 'node:vm';

const output = process.argv[2] || 'build/web';
const page = readFileSync(join(output, 'index.html'), 'utf8');
const loader = readFileSync(join(output, 'vc-web.js'), 'utf8');
// An optional source directory exercises an edit before rebuilding immutable
// web assets; the normal gate always reads the shipped build/web copy.
const source = readFileSync(join(process.argv[3] || output, 'vc-source.js'), 'utf8');
const { createSourcePanel, sourceLocation, walkCallers, recentLines,
  isSourceShortcut, SOURCE_SHORTCUT, formatSourceLine } =
  await import(`data:text/javascript,${encodeURIComponent(source)}`);
const lifecycleFailures = [];
function lifecycleCheck(name, check) {
  try { check(); console.log(`PASS ${name}`); }
  catch (error) {
    const failure = `${name}: ${error.message}`;
    lifecycleFailures.push(failure);
    console.error(`FAIL ${failure}`);
  }
}
const indexName = loader.match(/\.\/(source-index\.[0-9a-f]{12}\.json)/)?.[1];
assert.ok(indexName, 'page references its immutable source index');
const index = JSON.parse(readFileSync(join(output, indexName)));
const vcMap = JSON.parse(readFileSync(join(output, index.images['VC.OVL'].url)));

// The required adversarial stack uses a real VC return and a real code
// address which does NOT follow a CALL, plus garbage. Only the return wins.
const realCall = vcMap.calls.find(call => call[2] === 'near' && call[0] < 65536);
const ends = new Set(vcMap.calls.map(call => call[0]));
const nonReturn = vcMap.lines.find(row => row[0] > 0 && row[0] < 65536 && !ends.has(row[0]));
assert.ok(realCall && nonReturn);
const address = offset => ({ image: 'VC.OVL', offset });
const snapshot = { raw: { cs: 0x1000, ss: 0x2000, sp: 0x100 },
  current: { cs: 0x1000 }, stack: [realCall[0], nonReturn[0], 0xffff]
    .map((word, i) => ({ sp: 0x100 + 2 * i, word })) };
const callers = walkCallers(snapshot, name => name === 'VC.OVL' ? vcMap : null,
  (cs, ip) => cs === 0x1000 && ip !== 0xffff ? address(ip) : null);
assert.equal(callers.length, 1, 'data code pointers and garbage are not callers');
assert.equal(callers[0].offset, realCall[1], 'caller is the CALL, not its successor');
assert.equal(callers[0].returnOffset, realCall[0]);
console.log('PASS source return rule: real VC CALL return accepted, non-CALL code word and garbage rejected');

const file = { name: 'ORIGINAL.ASM', path: 'asm/ORIGINAL.ASM', url: 'original.0123456789ab.txt',
  via: [{ name: 'VCOVL.ASM', line: 3482 }, { name: 'OUTER.INC', line: 18 }] };
const fixture = { version: 1, image: 'FIXTURE.COM', kind: 'asm', files: [file],
  lines: Array.from({ length: 40 }, (_, i) => [i * 10, 0, i + 1, 3]),
  calls: [[103, 100, 'far'], [153, 150, 'near']] };
const farStack = { ...snapshot, stack: [103, 0x3000, 153]
  .map((word, i) => ({ sp: i * 2, word })) };
const farCallers = walkCallers(farStack, () => fixture,
  (cs, ip) => cs === 0x3000 ? { image: fixture.image, offset: ip } : null);
assert.deepEqual(farCallers.map(row => [row.offset, row.callKind]), [[100, 'far'], [150, 'near']],
  'a proved far return changes the CS used for subsequent near callers');
const recursive = { ...snapshot, stack: Array.from({ length: 20 }, (_, i) => ({ sp: i * 2, word: 153 })) };
assert.equal(walkCallers(recursive, () => fixture,
  (cs, ip) => cs === snapshot.current.cs ? { image: fixture.image, offset: ip } : null).length, 8);
assert.equal(sourceLocation(fixture, 165), null, 'unlisted gaps are never guessed');
const recent = Array.from({ length: 80 }, (_, i) => ({ image: fixture.image, offset: Math.floor(i / 2) * 10 }));
const distinct = recentLines(recent, () => fixture);
assert.equal(distinct.length, 32);
assert.deepEqual(distinct.map(row => row.line), Array.from({ length: 32 }, (_, i) => i + 1));
const cMap = { image: 'ROGUE.EXE', kind: 'functions', functions: [[20, 40, 'command_']] };
const fn = sourceLocation(cMap, 27);
assert.equal(fn.function, 'command_');
assert.equal(fn.offset, 7);
assert.equal(fn.line, undefined, 'compiled C must not invent source line numbers');
assert.match(fn.text, /C function, map file/);
const functions = Array.from({ length: 4096 }, (_, i) => [20 + i * 16, 28 + i * 16, `fn${i}_`]);
let functionReads = 0;
const largeMap = { ...cMap, functions: new Proxy(functions, { get(rows, key) {
  if (typeof key === 'string' && /^\d+$/.test(key)) functionReads++;
  return Reflect.get(rows, key);
} }) };
lifecycleCheck('Rogue function lookup is logarithmic', () => {
  assert.equal(sourceLocation(largeMap, functions.at(-1)[1] - 1).function, 'fn4095_');
  assert.ok(functionReads <= Math.ceil(Math.log2(functions.length)) + 2,
    `expected at most 14 function rows, read ${functionReads}`);
});
for (const offset of [19, 28, 29, functions.at(-1)[1]])
  assert.equal(sourceLocation({ ...cMap, functions }, offset), null, 'function gaps/end are not guessed');
assert.equal(sourceLocation({ ...cMap, functions }, 20).function, 'fn0_');
assert.equal(sourceLocation({ ...cMap, functions }, 36).function, 'fn1_');
assert.equal(sourceLocation({ ...cMap, functions: [] }, 20), null);

// VC.OVL's declared stack ends at 0900h. The live smoke regression had
// genuine callers below that bound, then old VC/Hack-looking words above
// it. Retained image bytes are valid copied-code evidence, not permission
// to scan beyond the active image's original stack allocation.
const stackMap = { ...fixture, image: 'STACK.EXE', stack: { segment: 0x300, top: 0x900 } };
const oldHack = { image: 'HACK.EXE', kind: 'functions', functions: [[0x1f000, 0x20000, 'buzz_']],
  calls: [[0x1f2c2, 0x1f2bd, 'far']] };
const stackAddress = offset => ({ image: stackMap.image, offset });
const boundedStack = { raw: { cs: 0x2000, ip: 160, ss: 0x2300, sp: 0x8c2 },
  current: { image: stackMap.image, cs: 0x2000, ip: 160, offset: 160 }, stack: [
    { sp: 0x8ca, word: 103, far: stackAddress(103), near: null },
    { sp: 0x8cc, word: 0x2000, far: null, near: null },
    { sp: 0x8f6, word: 153, far: null, near: stackAddress(153) },
    { sp: 0x976, word: 153, far: null, near: stackAddress(153) },
    { sp: 0xa64, word: 0xf302, far: { image: oldHack.image, offset: 0x1f2c2 }, near: null },
    { sp: 0xa66, word: 0x14b6, far: null, near: null },
  ] };
const stackMaps = name => name === stackMap.image ? stackMap : name === oldHack.image ? oldHack : null;
lifecycleCheck('declared MZ stack excludes stale VC/Hack caller words beyond its top', () => {
  assert.deepEqual(walkCallers(boundedStack, stackMaps, () => null)
    .map(row => [row.image, row.offset ?? row.address]),
  [[stackMap.image, 100], [stackMap.image, 150]]);
});
lifecycleCheck('a far return cannot read its CS beyond the declared stack top', () => {
  const straddling = { ...boundedStack, stack: [
    { sp: 0x8fe, word: 103, far: stackAddress(103), near: null },
    { sp: 0x900, word: 0x2000, far: null, near: null },
  ] };
  assert.deepEqual(walkCallers(straddling, stackMaps, () => null), []);
});
lifecycleCheck('an alternate stack segment is not bounded by the initial MZ stack', () => {
  const alternate = { ...boundedStack, raw: { ...boundedStack.raw, ss: 0x2400 },
    stack: [{ sp: 0xa00, word: 153, far: null, near: stackAddress(153) }] };
  assert.deepEqual(walkCallers(alternate, stackMaps, () => null).map(row => row.offset), [150]);
});
const included = { file: 'VCSUBS.INC', line: 2806, via: [{ name: 'VCOVL.ASM', line: 3482 }], text: 'RET' };
lifecycleCheck('source labels show only the actual file and line', () =>
  assert.equal(formatSourceLine(included), 'VCSUBS.INC:2806  RET'));
lifecycleCheck('tabs expand to eight source columns even on long lines', () => {
  assert.equal(formatSourceLine({ ...included, via: [], text: '\tMOV\tAX,1\t; Привет' }),
    'VCSUBS.INC:2806          MOV     AX,1    ; Привет');
  assert.equal(formatSourceLine({ ...included, via: [], text: `12345678\t${'x'.repeat(257)}\tEND` }),
    `VCSUBS.INC:2806  12345678        ${'x'.repeat(257)}       END`);
});
console.log('PASS source history/functions: 32 distinct newest-first lines, far/near stack chain, truthful C offsets');

class Node {
  constructor(tag, document) { this.tagName = tag; this.ownerDocument = document;
    this.children = []; this.listeners = new Map(); this.attributes = {}; this.hidden = true; this.text = '';
    this.replacements = 0; }
  set textContent(text) { this.text = String(text); this.children = []; }
  get textContent() { return this.text + this.children.map(child => child.textContent).join(''); }
  set innerHTML(_) { assert.fail('original source must never enter innerHTML'); }
  appendChild(child) { this.children.push(child); return child; }
  replaceChildren(...children) { this.replacements++; this.text = ''; this.children = children; }
  setAttribute(name, value) { this.attributes[name] = value; }
  addEventListener(name, callback) { this.listeners.set(name, callback); }
  removeEventListener(name) { this.listeners.delete(name); }
}
const document = { createElement: tag => new Node(tag, document) };
const button = document.createElement('button'), panel = document.createElement('aside');
const textarea = document.createElement('textarea');
const texts = Array.from({ length: 40 }, (_, i) => i === 16
  ? '\tMOV AX,1 ; <img src=x onerror="throw 1"> Привет' : `\tMOV AX,${i} ; comment ${i + 1}`);
const assets = {
  'source-index.0123456789ab.json': JSON.stringify({ version: 1, images: { [fixture.image]: { url: 'fixture.0123456789ab.json' } } }),
  'fixture.0123456789ab.json': JSON.stringify(fixture),
  [file.url]: texts.join('\r\n'),
};
const requests = [];
let snapshots = 0;
const focusCalls = [];
textarea.focus = options => { focusCalls.push(options); document.activeElement = textarea; };
let current = { ...snapshot, current: { image: fixture.image, offset: 160, cs: 0x1000, ip: 160, kind: 'instruction' },
  stack: [], recent };
const binding = createSourcePanel({ button, panel, indexURL: 'https://local.invalid/source-index.0123456789ab.json',
  // Keep the old callback connected while proving the regression: the old
  // panel must fail when it calls that unconditional focus path.
  focus: options => textarea.focus(options),
  getTextarea: () => textarea,
  getSnapshot() { snapshots++; return current; },
  resolveAddress: () => null,
  async fetchFile(url) {
    requests.push(url);
    const name = new URL(url).pathname.slice(1);
    assert.ok(Object.hasOwn(assets, name), `unexpected source asset ${name}`);
    return { ok: true, arrayBuffer: async () => new TextEncoder().encode(assets[name]).buffer };
  },
});
try {
  assert.equal(requests.length, 0, 'closed panel fetches nothing');
  assert.equal(snapshots, 0, 'closed panel does not copy guest state');
  await binding.toggle();
  assert.equal(binding.view.status, 'ready');
  assert.equal(binding.view.current.text, texts[16]);
  assert.equal(binding.view.now.length, 17);
  assert.equal(binding.view.now[0].line, 9);
  assert.equal(binding.view.now[16].line, 25);
  assert.ok(panel.textContent.includes(texts[16].trimStart()), 'markup and Cyrillic remain literal original text');
  const nodes = node => [node, ...node.children.flatMap(nodes)];
  const highlighted = nodes(panel).filter(node => node.className === 'source-current');
  assert.equal(highlighted.length, 1);
  lifecycleCheck('rendered include chains are title attributes, not repeated text', () => {
    assert.equal(highlighted[0].attributes.title, 'VCOVL.ASM:3482 → OUTER.INC:18 → ORIGINAL.ASM:17');
    assert.ok(highlighted[0].textContent.startsWith('ORIGINAL.ASM:17  '));
    assert.doesNotMatch(highlighted[0].textContent, /VCOVL\.ASM|OUTER\.INC|\t/);
    assert.equal(highlighted[0].textContent, `ORIGINAL.ASM:17          ${texts[16].trimStart()}`);
    for (const row of nodes(panel).filter(node => /^source-(?:line|current)$/.test(node.className)))
      assert.match(row.attributes.title, /^VCOVL\.ASM:3482 → OUTER\.INC:18 → ORIGINAL\.ASM:\d+$/);
  });
  assert.equal(button.attributes['aria-expanded'], 'true');
  const initialReplacements = panel.replacements, selectedBlock = panel.children[1];
  await binding.refresh();
  current = structuredClone(current);
  await binding.refresh();
  lifecycleCheck('unchanged and cloned snapshots preserve source selection nodes', () => {
    assert.equal(panel.replacements, initialReplacements, 'unchanged polls must not replaceChildren');
    assert.equal(panel.children[1], selectedBlock, 'selected text stays in the same DOM nodes');
  });
  let rendered = panel.replacements;
  current.current.offset = 170; current.current.ip = 170;
  await binding.refresh();
  assert.equal(panel.replacements, rendered + 1, 'an in-place current-address change redraws');
  assert.equal(binding.view.current.line, 18);
  rendered = panel.replacements;
  current.stack = [{ sp: 0x100, word: 153, near: { image: fixture.image, offset: 153 } }];
  await binding.refresh();
  assert.equal(panel.replacements, rendered + 1, 'a caller-only change redraws');
  assert.equal(binding.view.callers[0].line, 16);
  rendered = panel.replacements;
  current.recent = [{ image: fixture.image, offset: 220 }, ...current.recent];
  await binding.refresh();
  assert.equal(panel.replacements, rendered + 1, 'a history-only change redraws');
  assert.equal(binding.view.recent[0].line, 23);
  const readCount = snapshots, fetchCount = requests.length;
  binding.close();
  lifecycleCheck('programmatic close never opens the phone keyboard', () => assert.equal(focusCalls.length, 0));
  await binding.refresh();
  assert.equal(snapshots, readCount, 'closing stops snapshots');
  assert.equal(panel.hidden, true);
  await binding.toggle();
  assert.equal(binding.view.status, 'ready', 'opening the same snapshot must not stay on Loading');
  assert.equal(requests.length, fetchCount, 'second open reuses source/map downloads');
  binding.close();
  focusCalls.length = 0;
  for (const expectedOpen of [true, false]) {
    document.activeElement = null;
    button.listeners.get('pointerdown')({ preventDefault() {} });
    button.listeners.get('click')();
    await binding.refresh();
    assert.equal(binding.opened, expectedOpen);
  }
  lifecycleCheck('unfocused phone taps never focus the terminal on open or close', () => assert.equal(focusCalls.length, 0));
  focusCalls.length = 0;
  for (const expectedOpen of [true, false]) {
    document.activeElement = textarea;
    button.listeners.get('pointerdown')({ preventDefault() {}, pointerType: 'mouse' });
    document.activeElement = null; // Focus at pointerdown, not click, is what counts.
    button.listeners.get('click')();
    await binding.refresh();
    assert.equal(binding.opened, expectedOpen);
  }
  lifecycleCheck('focused pointerdown refocuses once per tap without scrolling', () =>
    assert.deepEqual(focusCalls, [{ preventScroll: true }, { preventScroll: true }]));
  focusCalls.length = 0;
  for (const expectedOpen of [true, false]) {
    // Android focuses the textarea at load without a keyboard. A focus()
    // call inside a later tap opens the keyboard, so touch never refocuses.
    document.activeElement = textarea;
    button.listeners.get('pointerdown')({ preventDefault() {}, pointerType: 'touch' });
    button.listeners.get('click')();
    await binding.refresh();
    assert.equal(binding.opened, expectedOpen);
  }
  lifecycleCheck('touch taps on Source never call focus, even with the textarea focused', () =>
    assert.equal(focusCalls.length, 0));
  await binding.toggle();
  let selection = '';
  document.getSelection = () => ({ toString: () => selection });
  focusCalls.length = 0;
  document.activeElement = textarea;
  panel.listeners.get('pointerdown')({ pointerType: 'mouse' });
  document.activeElement = null; // a mouse press on the panel moves focus to the page
  panel.listeners.get('pointerup')({ pointerType: 'mouse' });
  lifecycleCheck('a mouse click in the open panel gives keys back to VC', () =>
    assert.deepEqual(focusCalls, [{ preventScroll: true }]));
  focusCalls.length = 0;
  document.activeElement = textarea;
  panel.listeners.get('pointerdown')({ pointerType: 'mouse' });
  document.activeElement = null;
  selection = 'MOV AX,1';
  panel.listeners.get('pointerup')({ pointerType: 'mouse' });
  lifecycleCheck('selecting panel text keeps the selection instead of refocusing VC', () =>
    assert.equal(focusCalls.length, 0));
  selection = '';
  binding.close();
  focusCalls.length = 0;
  document.activeElement = null;
  button.listeners.get('pointerdown')({ preventDefault() {} });
  document.activeElement = textarea;
  button.listeners.get('click')();
  await binding.refresh();
  lifecycleCheck('focus acquired after pointerdown does not authorize refocusing', () => assert.equal(focusCalls.length, 0));
  binding.close();
  await binding.toggle();
  current = { ...current, current: { ...current.current, image: 'UNMAPPED.COM' } };
  await binding.refresh();
  assert.equal(binding.view.status, 'no-map');
  assert.equal(panel.children.length, 1, 'unmapped program is explained in one line');
  assert.match(panel.textContent, /No source map for UNMAPPED.COM/);
  rendered = panel.replacements;
  current = structuredClone(current);
  await binding.refresh();
  lifecycleCheck('unchanged no-map snapshots also keep their DOM nodes', () => assert.equal(panel.replacements, rendered));
} finally { binding.dispose(); }
console.log('PASS source panel: lazy cached fetch, 17-line context, highlight, plain text, unknown-map message, closed-state isolation');

assert.equal(SOURCE_SHORTCUT, 'Ctrl-Shift-F12');
const chord = { type: 'keydown', key: 'F12', code: 'F12', ctrlKey: true, shiftKey: true, altKey: false, metaKey: false };
assert.ok(isSourceShortcut(chord));
for (const other of [{ ...chord, altKey: true }, { ...chord, ctrlKey: false },
  { ...chord, code: 'KeyS', key: 's', altKey: true, shiftKey: false }]) assert.equal(isSourceShortcut(other), false);
// Exercise the actual eager page handler as well as the lazy controller:
// repeat and key-up (even after releasing modifiers first) must not leak F12.
const handlerText = loader.slice(loader.indexOf('let sourceShortcutHeld ='),
  loader.indexOf('window.addEventListener("keydown", sourceKey'));
assert.ok(handlerText.includes('function sourceKey'));
let toggles = 0, prevented = 0, stopped = 0;
const handler = runInNewContext(`${handlerText}; sourceKey`, { exited: false, toggleSource() { toggles++; } });
const event = extra => ({ ...chord, repeat: false, preventDefault() { prevented++; },
  stopImmediatePropagation() { stopped++; }, ...extra });
handler(event({}));
handler(event({ repeat: true }));
handler(event({ type: 'keyup', ctrlKey: false, shiftKey: false }));
assert.equal(toggles, 1);
assert.equal(prevented, 3);
assert.equal(stopped, 3);
handler(event({ ctrlKey: false, shiftKey: false }));
assert.equal(toggles, 1, 'plain F12 is not the Source shortcut');
assert.match(page, /title="[^"]*Ctrl-Shift-F12/);
console.log('PASS source shortcut: actual page handler consumes only the free chord, repeats and release without guest input');

assert.match(source, /setInterval\([^]*?100\)/, 'the open panel refreshes even for keys which do not redraw VC');

// A pending dynamic import is outside the panel's own generation guard.
// Exercise the actual loader function with only its transport substituted.
const toggleCode = loader.slice(loader.indexOf('async function toggleSource()'), loader.indexOf('const firstSourceClick'))
  .replace(/import\("\.\/vc-source\.js\?v=[^"]+"\)/, 'loadModule()')
  .replaceAll('import.meta.url', '"https://local.invalid/vc-web.js"');
async function deferredLoad(fail) {
  let release, reject;
  let toggled = 0, constructed = 0;
  const context = { vc: {}, exited: false, sourceLoad: undefined, sourcePanel: undefined,
    sourceModule: undefined, sourceElement: { hidden: true }, sourceIndexURL: 'https://local.invalid/index.json',
    sourceButton: { attributes: {}, title: '', removeEventListener() {},
      removeAttribute(name) { delete this.attributes[name]; },
      setAttribute(name, value) { this.attributes[name] = value; } },
    firstSourceClick() {}, layoutSource() {}, terminal: { focus() {} }, URL,
    loadModule: () => new Promise((yes, no) => { release = yes; reject = no; }) };
  const toggle = runInNewContext(`${toggleCode}; toggleSource`, context);
  const pending = toggle();
  if (fail) reject(new Error('offline module'));
  else {
    context.exited = true;
    release({ createSourcePanel() { constructed++; return { async toggle() { toggled++; } }; } });
  }
  await pending;
  return { ...context, toggled, constructed };
}
const exitedLoad = await deferredLoad(false);
lifecycleCheck('late import cannot reopen after VC exits', () => {
  assert.equal(exitedLoad.toggled, 0);
  assert.equal(exitedLoad.constructed, 0);
});
const failedLoad = await deferredLoad(true);
lifecycleCheck('module error never overlays the guest screen', () => {
  assert.equal(failedLoad.sourceElement.hidden, true);
  assert.match(failedLoad.sourceButton.title, /offline module/);
  assert.equal(failedLoad.sourceButton.attributes['aria-expanded'], 'false');
});
assert.equal(lifecycleFailures.length, 0, lifecycleFailures.join('\n'));
console.log('PASS source lifecycle: focus preserved, late import ignored after quit, unobtrusive import failure');
