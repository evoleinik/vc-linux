// The real translated VC in Emscripten's MEMFS, with the same byte hooks as
// the page. Actions wait for screen state; timers only put a bound on failure.
// Add --fetch-failure, --fetch-timeout or --memory-limit for recovery gates.
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { isMainThread, parentPort, Worker } from 'node:worker_threads';

function exitAfterOutput(code) {
  // Worker and main-thread stdout can both be asynchronous when CI captures
  // them. Do not discard the final PASS line or failure diagnostics on exit.
  process.stdout.write('', () => process.stderr.write('', () => process.exit(code)));
}

const timeout = Number(process.env.WEB_SMOKE_TIMEOUT || 10000);
if (isMainThread) {
  // A missing Asyncify yield freezes the guest's JS event loop, including
  // its normal timeout. Keep the failure watchdog outside that thread.
  const worker = new Worker(new URL(import.meta.url), { argv: process.argv.slice(2) });
  let deadline;
  const arm = (description) => {
    clearTimeout(deadline);
    deadline = setTimeout(() => {
      console.error(`FAIL: guest event loop stalled while waiting for ${description}`);
      worker.terminate().then(() => exitAfterOutput(1));
    }, timeout + 1000);
  };
  arm('startup');
  worker.on('message', arm);
  worker.on('error', (error) => { console.error(error); exitAfterOutput(1); });
  worker.on('exit', (code) => { clearTimeout(deadline); exitAfterOutput(code); });
  await new Promise(() => {});
}

const moduleArgument = process.argv.slice(2).find((argument) => !argument.startsWith('--'));
const modulePath = moduleArgument
  ? resolve(moduleArgument)
  : fileURLToPath(new URL('../build/web/vc.mjs', import.meta.url));
const fetchFailure = process.argv.includes('--fetch-failure');
const fetchTimeout = process.argv.includes('--fetch-timeout');
const memoryLimit = process.argv.includes('--memory-limit');
const programDeadlines = [];
const realSetTimeout = globalThis.setTimeout;
if (fetchTimeout) {
  globalThis.setTimeout = (callback, delay, ...args) => {
    // Advance only the production download deadline, not guest idle waits
    // or test watchdogs. Check its actual 30-second default/cap without
    // spending a minute waiting for two deliberately stalled downloads.
    if (delay >= 30000) {
      programDeadlines.push(delay);
      return realSetTimeout(callback, 100, ...args);
    }
    return realSetTimeout(callback, delay, ...args);
  };
}
const moduleURL = pathToFileURL(modulePath);
const loader = readFileSync(resolve(dirname(modulePath), 'vc-web.js'), 'utf8');
const buildHash = loader.match(/locateFile:[^\n]*\?v=([0-9a-f]+)/)?.[1];
assert.ok(buildHash, 'the built page must supply a versioned wasm URL resolver');
const { default: createVC } = await import(pathToFileURL(modulePath));
const screenPath = '/tmp/vc-screen.txt';
const encoder = new TextEncoder();
const observers = new Set();
let vc;
let screen = '';
let pending = new Uint8Array();
let inputReads = 0;
let outputCalls = 0;
let startupHeapBytes = 0;
let startupHeapTop = 0;
let wasmExports;
let exitCode;
let exitCalls = 0;
let stage = '1 startup';
let sourcePanel;
let sourceButton;
let sourceElement;
let sourceSnapshot;
let sourceResolved;
let sourceDraws = 0;
let sourcePageKey;
let sourceKeyReducer;
let sourceInitialInput;
const sourceFetches = [];
const originalSources = new Map();
const programModules = ['gwbasic.wasm', 'bootlogo.wasm', 'rogue.wasm', 'vz.wasm'];
const programFiles = new Map(readdirSync(dirname(modulePath)).flatMap((name) => {
  const match = name.match(/^(gwbasic|bootlogo|rogue|vz)\.([0-9a-f]{12})\.wasm$/);
  return match ? [[`${match[1]}.wasm`, name]] : [];
}));
assert.equal(programFiles.size, 4, 'all side modules have immutable build-hash filenames');
const moduleFetches = [];
const expectedFetches = [];
let heldDownload = null;
const speakerEvents = [];
const faultMessage = 'No translated code at 0000:0000. GWBASIC.EXE stopped.';
let sawFaultMessage = false;
let graphics = null;
let graphicsFrames = 0;
let textTransitions = 0;
const framePath = '/tmp/vc-frame.pgm';
// Recognize only font-aligned text, directly from the public-domain source
// font, so graphics-mode waits observe the same pixels as the canvas.
const font = new Map();
const fontHeader = readFileSync(new URL('../third_party/font8x8/font8x8_basic.h', import.meta.url), 'utf8');
for (const [, values, point] of fontHeader.matchAll(/\{([^}]+)\},\s*\/\/ U\+([0-9A-Fa-f]{4})/g)) {
  const code = parseInt(point, 16);
  if (code >= 32 && code < 127) {
    const rows = [...values.matchAll(/0x([0-9A-Fa-f]+)/g)].map((m) => parseInt(m[1], 16));
    font.set(rows.join(','), String.fromCharCode(code));
  }
}
function graphicsText(frame) {
  if (!frame) return '';
  return Array.from({ length: 25 }, (_, row) => Array.from({ length: frame.width / 8 }, (_, col) => {
    const glyph = Array.from({ length: 8 }, (_, y) => {
      let bits = 0;
      for (let x = 0; x < 8; ++x)
        if (frame.pixels[(row * 8 + y) * frame.width + col * 8 + x]) bits |= 1 << x;
      return bits;
    });
    return font.get(glyph.join(',')) ?? '?';
  }).join('')).join('\n');
}
const pixel = (x, y) => graphics?.pixels[y * graphics.width + x];

function observe() {
  if (vc) {
    try {
      screen = vc.FS.readFile(screenPath, { encoding: 'utf8' });
      if (screen.includes(faultMessage)) sawFaultMessage = true;
    } catch (error) {
      if (error.code !== 'ENOENT' && error.errno !== 44) throw error;
    }
  }
  for (const check of [...observers]) check();
}

function send(data) {
  const bytes = encoder.encode(data);
  const next = new Uint8Array(pending.length + bytes.length);
  next.set(pending);
  next.set(bytes, pending.length);
  pending = next;
}

function until(description, predicate) {
  parentPort.postMessage(description);
  return new Promise((resolveWait, reject) => {
    const finish = (error) => {
      clearTimeout(deadline);
      observers.delete(check);
      if (error) reject(error);
      else resolveWait(screen);
    };
    const check = () => {
      if (predicate(screen)) finish();
    };
    // Never accelerate the harness watchdog, even when a slow-run timeout
    // equals the production download deadline being tested.
    const deadline = realSetTimeout(() => {
      finish(new Error(`Timed out waiting for ${description}`));
    }, timeout);
    observers.add(check);
    observe();
  });
}

// Only DOM storage and events are doubled. The published Source controller,
// mapper, fetch path and renderer run unchanged against the real guest CPU.
function sourceNode(document, tagName) {
  const listeners = new Map();
  const attributes = new Map();
  const classes = new Set();
  let ownText = '';
  return {
    ownerDocument: document, tagName, children: [], hidden: false, dataset: {},
    className: '',
    classList: {
      add: (...names) => names.forEach((name) => classes.add(name)),
      remove: (...names) => names.forEach((name) => classes.delete(name)),
      contains: (name) => classes.has(name),
      toggle(name, force = !classes.has(name)) {
        if (force) classes.add(name);
        else classes.delete(name);
        return force;
      },
    },
    append(...nodes) { this.children.push(...nodes); },
    appendChild(node) { this.append(node); return node; },
    replaceChildren(...nodes) { ownText = ''; this.children = [...nodes]; },
    get textContent() {
      return ownText + this.children.map((node) => typeof node === 'string' ? node : node.textContent).join('');
    },
    set textContent(value) { ownText = String(value); this.children = []; },
    get innerHTML() { throw new Error('Source must never use innerHTML'); },
    set innerHTML(_value) { throw new Error('Original source must be rendered as plain text, never HTML'); },
    setAttribute(name, value) { attributes.set(name, String(value)); },
    getAttribute(name) { return attributes.get(name) ?? null; },
    removeAttribute(name) { attributes.delete(name); },
    addEventListener(name, callback) {
      if (!listeners.has(name)) listeners.set(name, new Set());
      listeners.get(name).add(callback);
    },
    removeEventListener(name, callback) { listeners.get(name)?.delete(callback); },
    dispatch(name, event = {}) {
      for (const callback of listeners.get(name) || [])
        callback({ type: name, target: this, preventDefault() {}, ...event });
    },
  };
}

function takeSourceSnapshot() {
  sourceSnapshot = undefined;
  vc._vc_source_snapshot();
  assert.equal(sourceSnapshot?.version, 1, 'the real wasm publishes a read-only guest snapshot');
  return sourceSnapshot;
}

function resolveSourceAddress(cs, ip, preceding = false) {
  sourceResolved = undefined;
  vc._vc_source_resolve(cs, ip, Number(preceding));
  assert.notEqual(sourceResolved, undefined, 'the wasm address resolver answers synchronously');
  return sourceResolved;
}

async function installSourcePanel() {
  const sourceScript = readFileSync(join(dirname(modulePath), 'vc-source.js'), 'utf8');
  const { createSourcePanel } = await import(`data:text/javascript,${encodeURIComponent(sourceScript)}`);
  const indexName = loader.match(/source-index\.[0-9a-f]{12}\.json(?:\?v=[0-9a-f]+)?/)?.[0];
  assert.ok(indexName, 'the published loader names an immutable source index');
  const indexURL = new URL(indexName, moduleURL);
  const document = { createElement: (tagName) => sourceNode(document, tagName) };
  sourceButton = document.createElement('button');
  sourceElement = document.createElement('aside');
  sourceElement.hidden = true;
  sourcePanel = createSourcePanel({
    button: sourceButton, panel: sourceElement,
    getSnapshot: takeSourceSnapshot, resolveAddress: resolveSourceAddress, indexURL,
    async fetchFile(url) {
      const resource = new URL(url, indexURL);
      assert.equal(resource.protocol, 'file:', 'Source smoke reads only local published files');
      assert.equal(dirname(fileURLToPath(resource)), dirname(modulePath), 'source requests stay in the web build');
      assert.match(basename(resource.pathname), /\.[0-9a-f]{12}\.(?:json|txt)$/,
        'source maps and text have immutable build-hash names');
      const bytes = await readFile(resource);
      sourceFetches.push({ url: resource.href, bytes: bytes.length });
      return new Response(bytes, { status: 200 });
    },
    onChange() { sourceDraws++; observe(); },
  });
  const keypadScript = readFileSync(join(dirname(modulePath), 'vc-keypad.js'), 'utf8');
  const { initialInput, reduceInput } = await import(`data:text/javascript,${encodeURIComponent(keypadScript)}`);
  sourceInitialInput = initialInput;
  sourceKeyReducer = reduceInput;
  // Execute the published capture handler itself, including its keyup/repeat
  // latch. Only its DOM listener registration and lazy import are outside
  // this Node harness; the already-created production panel is the target.
  const captureCode = loader.match(/let sourceShortcutHeld = false;[\s\S]*?(?=\nwindow\.addEventListener\("keydown", sourceKey)/)?.[0];
  assert.ok(captureCode, 'the page captures the Source shortcut before xterm');
  sourcePageKey = new Function('toggleSource', 'exited', `${captureCode}\nreturn sourceKey;`)(
    () => sourcePanel.toggle(), false);
}

function originalSource(path) {
  assert.match(path, /^(?:asm|third_party)\//, 'source provenance names a vendored original');
  if (!originalSources.has(path)) {
    const bytes = readFileSync(new URL(`../${path}`, import.meta.url));
    let decoded;
    try { decoded = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
    catch {
      const encoding = path.startsWith('third_party/vzeditor/') ? 'shift_jis' : 'ibm866';
      const decoder = new TextDecoder(encoding, { fatal: true });
      // ICU swaps a few legacy control characters (notably DOS EOF 1Ah).
      // Preserve those bytes before decoding possibly multibyte comments.
      decoded = bytes.toString('latin1').split(/([\x00-\x1f\x7f])/).map((part) =>
        part.length === 1 && (part.charCodeAt(0) < 32 || part.charCodeAt(0) === 127)
          ? part : decoder.decode(Buffer.from(part, 'latin1'))).join('');
    }
    originalSources.set(path, decoded.split('\n').map((line) => line.replace(/\r$/, '')));
  }
  return originalSources.get(path);
}

function assertSourceLine(line) {
  assert.ok(line && typeof line.path === 'string', 'an assembly row identifies its original source file');
  assert.ok(Number.isInteger(line.line) && line.line > 0, 'source line numbers are one-based integers');
  assert.equal(line.file, basename(line.path), 'the displayed filename is the actual source filename');
  assert.equal(line.text, originalSource(line.path)[line.line - 1], `${line.path}:${line.line} matches the original, including comments`);
  let expanded = '';
  for (const character of line.text)
    expanded += character === '\t' ? ' '.repeat(8 - expanded.length % 8) : character;
  const descendants = node => [node, ...node.children.flatMap(descendants)];
  const rows = descendants(sourceElement).filter(node =>
    /^source-(?:line|current)$/.test(node.className)
    && node.textContent === `${line.file}:${line.line}  ${expanded}`);
  assert.ok(rows.length, 'a visible row contains only its file:line and literal source with eight-column tabs');
  if (line.via?.length) {
    const chain = [...line.via.map(origin => `${origin.name}:${origin.line}`), `${line.file}:${line.line}`].join(' → ');
    assert.ok(rows.some(row => row.getAttribute('title') === chain), 'the matching source row keeps its include chain in its title');
  }
  for (const [index, origin] of (line.via || []).entries()) {
    assert.equal(origin.name, basename(origin.path), 'include provenance names the actual parent source');
    const directive = originalSource(origin.path)[origin.line - 1];
    assert.match(directive, /\bINCLUDE\b/i, 'include provenance points to the original INCLUDE directive');
    assert.ok(directive.toUpperCase().includes((line.via[index + 1]?.name || line.file).toUpperCase()),
      'the original parent directive includes this child source file');
  }
}

function assertVCSource(view) {
  assert.ok(['VC.COM', 'VC.OVL'].includes(view.current.image), 'Now belongs to the running VC image');
  assertSourceLine(view.current);
  for (const line of [view.current, ...view.callers]) {
    const roots = [line.file, ...(line.via || []).map((via) => via.name)];
    assert.ok(roots.some((name) => name === 'VC.ASM' || name === 'VCOVL.ASM'),
      'each VC row truthfully names VC.ASM/VCOVL.ASM, including include-file provenance');
  }
  assert.ok(view.callers.length > 0 && view.callers.length <= 8, 'Called from shows one to eight real CALL sites');
  for (const caller of view.callers) {
    assert.ok(['VC.COM', 'VC.OVL'].includes(caller.image), 'a VC caller belongs to VC');
    assertSourceLine(caller);
  }
  assert.ok(view.now.length >= 9 && view.now.length <= 17, 'Now includes about eight source lines either side');
  for (const line of view.now) assertSourceLine(line);
  assert.ok(view.recent.length > 0 && view.recent.length <= 32, 'Just ran is bounded to 32 entered source lines');
  assert.equal(new Set(view.recent.map((line) => `${line.path}:${line.line}`)).size, view.recent.length,
    'Just ran contains distinct source lines');
  for (const line of view.recent) assertSourceLine(line);
  for (const heading of ['Now', 'Called from', 'Just ran'])
    assert.ok(sourceElement.textContent.includes(heading), `the panel renders ${heading}`);
  const nodes = (node) => [node, ...node.children.flatMap((child) => typeof child === 'string' ? [] : nodes(child))];
  const highlighted = nodes(sourceElement).filter((node) => node.className === 'source-current');
  assert.equal(highlighted.length, 1, 'exactly the current source line is highlighted');
  assert.ok(highlighted[0].textContent.includes(`${view.current.file}:${view.current.line}`));
}

async function openSource(image) {
  const images = Array.isArray(image) ? image : [image];
  sourceButton.dispatch('click');
  await until(`Source panel maps ${images.join(' or ')}`, () => sourcePanel.opened
    && images.includes(sourcePanel.view?.current?.image));
  assert.equal(sourceElement.hidden, false, 'the Source button reveals the real panel');
  return sourcePanel.view;
}

function sourceShortcut() {
  let prevented = 0;
  const consumed = sourcePanel.handleKey({
    type: 'keydown', key: 'F12', code: 'F12', ctrlKey: true, altKey: false,
    shiftKey: true, metaKey: false, repeat: false,
    preventDefault() { prevented++; }, stopPropagation() {}, stopImmediatePropagation() {},
  });
  assert.equal(consumed, true, 'Ctrl-Shift-F12 is consumed by the Source panel');
  assert.ok(prevented > 0, 'the Source shortcut prevents the browser default');
}

async function closeSourceWithShortcut() {
  const before = screen;
  const reads = inputReads;
  const bytes = pending.slice();
  sourceShortcut();
  assert.equal(sourcePanel.opened, false, 'the same shortcut closes Source');
  assert.equal(sourceElement.hidden, true);
  assert.deepEqual(pending, bytes, 'toggling Source queues no DOS key');
  await until('the guest polls normally after Source closes', () => inputReads > reads);
  assert.equal(screen, before, 'closing Source preserves every VC screen row and its key bar');
}

async function checkSourceModifierRelease() {
  let state = sourceInitialInput();
  const pageKey = (type, key, flags, repeat = false) => {
    let captured = false;
    const event = {
      type, key, code: key === 'Control' ? 'ControlLeft' : key === 'Shift' ? 'ShiftLeft' : key,
      ctrlKey: !!(flags & 4), shiftKey: !!(flags & 1), altKey: false, metaKey: false, repeat,
      preventDefault() {}, stopImmediatePropagation() { captured = true; },
    };
    sourcePageKey(event);
    if (key === 'F12') assert.equal(captured, true, 'the real page capture keeps every Source F12 event out of DOS');
    if (!captured) {
      const result = sourceKeyReducer(state, { type: 'key', event });
      state = result.state;
      assert.equal(result.handled, true, 'modifier events use the real shared keyboard reducer');
      send(result.bytes);
    }
  };
  for (const releaseFirst of ['Control', 'Shift']) {
    const before = screen;
    pageKey('keydown', 'Control', 4);
    await until('the real Ctrl press changes VC\'s key bar', (text) =>
      text.split('\n')[24] !== before.split('\n')[24]);
    const reads = inputReads;
    pageKey('keydown', 'Shift', 5);
    await until('the real combined modifier report reaches VC', () => pending.length === 0 && inputReads > reads + 1);
    const modified = screen;
    const wasOpen = sourcePanel.opened;
    pageKey('keydown', 'F12', 5);
    await until(`the page Ctrl-Shift-F12 handler ${wasOpen ? 'closes' : 'opens'} Source`, () =>
      sourcePanel.opened !== wasOpen && (wasOpen || sourcePanel.view?.status === 'ready'));
    assert.equal(screen, modified, 'toggling with physical modifiers leaves the guest screen untouched');
    pageKey('keydown', 'F12', 5, true);
    assert.equal(sourcePanel.opened, !wasOpen, 'holding the shortcut does not toggle repeatedly');
    pageKey('keyup', 'F12', 5);
    const releaseReads = inputReads;
    pageKey('keyup', releaseFirst, releaseFirst === 'Control' ? 1 : 4);
    await until(`VC processes the ${releaseFirst} release before the other modifier`, () =>
      pending.length === 0 && inputReads > releaseReads + 1);
    pageKey('keyup', releaseFirst === 'Control' ? 'Shift' : 'Control', 0);
    await until(`VC's exact screen/key bar after releasing ${releaseFirst} first`, (text) => text === before);
    assert.deepEqual(state, sourceInitialInput(), 'neither modifier remains held after the Source shortcut');
  }
  assert.equal(sourcePanel.opened, false, 'two physical shortcuts opened and closed Source');
}

function assertFetches() {
  assert.deepEqual(moduleFetches.map(({ name }) => name), expectedFetches,
    'programs fetch only at their first EXEC; subsequent runs use the loaded module');
}

async function programRequest(url) {
  // Record even malformed/unexpected requests before an assertion can
  // reject: the production loader deliberately catches transport errors.
  const index = moduleFetches.length;
  const request = { name: String(url), url, bodyReads: 0 };
  moduleFetches.push(request);
  const resource = new URL(url);
  const filename = basename(resource.pathname);
  const name = filename.replace(/\.[0-9a-f]{12}\.wasm$/, '.wasm');
  request.name = name;
  request.filename = filename;
  assert.equal(resource.protocol, 'file:', 'Node tests read only local program files');
  assert.equal(resource.search, `?v=${buildHash}`, 'every lazy request has the page build hash');
  assert.ok(programModules.includes(name), `unexpected lazy asset ${name}`);
  assert.equal(filename, programFiles.get(name), 'main requests the exact shipped immutable module filename');
  assert.equal(name, expectedFetches[index], 'the request belongs to this EXEC');
  const attempt = moduleFetches.filter((entry) => entry.name === name).length;
  const ready = new Promise((release) => {
    heldDownload = { name, release, screen, inputReads, outputCalls };
  });
  observe();
  await ready;
  heldDownload = null;
  return { request, resource, attempt };
}

const programBytes = async (resource) => new Uint8Array(await readFile(resource));

async function fetchProgramBytes(url) {
  const { resource } = await programRequest(url);
  return programBytes(resource);
}

async function releaseProgramFetch(name) {
  await until(`${name} first-use download request`, () => heldDownload?.name === name);
  assertFetches();
  const held = heldDownload;
  // This checkpoint cannot run while a synchronous guest blocks Node. Keep
  // the actual module bytes withheld until it proves Asyncify has yielded.
  await new Promise((done) => setImmediate(done));
  observe();
  assert.equal(screen, held.screen, 'the guest screen stays unchanged while the download is held');
  assert.equal(inputReads, held.inputReads, 'the guest is suspended, not polling while downloading');
  assert.equal(outputCalls, held.outputCalls, 'a held download produces no new guest output');
  assert.equal(exitCalls, 0, 'waiting for a module must not terminate VC');
  held.release();
  console.log(`PASS lazy: ${programFiles.get(name)}?v=${buildHash} first EXEC waits quietly and yields to Node`);
}

async function basicLine(command) {
  // GW-BASIC's own small type-ahead buffer can overflow on a long paste.
  // Wait for each chunk's actual glyphs, never on typing-speed timers.
  const compact = (text) => text.replace(/\s/g, '');
  for (let start = 0; start < command.length; start += 8) {
    send(command.slice(start, start + 8));
    const expected = compact(command.slice(0, start + 8));
    await until(`BASIC input echo through character ${start + 8}`, (text) =>
      compact(graphics ? graphicsText(graphics) : text).includes(expected));
  }
  send('\r');
}

const hasPanels = (text) => text.startsWith('╔') && (text.split('\n')[24] || '').includes('5Copy')
  && text.includes('10Quit');
const isPanel = (text) => hasPanels(text) && text.includes('README');
// The shipped VC.INI selects the right panel (Active=1); its H: listing is
// independent of the left panel's remembered C: directory.
const selected = (text) => (text.split('\n')[21] || '').slice(40);
const commandLine = (text) => text.split('\n')[23] || '';
const rogueStatus = /Level:\s*(\d+)\s+Gold:\s*\d+\s+Hp:\s*\d+\(\s*\d+\)/;
const roguePlayers = (text) => text.split('\n').flatMap((line, row) =>
  row > 0 && !rogueStatus.test(line)
    ? Array.from(line).flatMap((char, col) => char === '@' ? [[row, col]] : []) : []);
const isRogue = (text) => rogueStatus.test(text) && roguePlayers(text).length === 1 && text.includes('.');
const rogueState = (text) => text.split('\n').slice(1).join('\n');

async function quitRogue() {
  send('Q');
  await until('Rogue really quit? confirmation', (text) => text.toLowerCase().includes('really quit?'));
  assert.ok(!hasPanels(screen), 'Q asks before ending the game');
  send('y');
  await until('VC panels after Rogue Q then y', hasPanels);
  assert.equal(exitCalls, 0, 'quitting Rogue must not quit VC');
}

async function selectFile(name) {
  // The status row, not the listing (where every name is always visible),
  // identifies the active selection. Advance only after the last key landed.
  for (let step = 0; step < 24; step++) {
    const previous = selected(screen);
    if (previous.includes(name)) return;
    send('\x1b[B');
    await until('the selection to move', (text) => selected(text) !== previous);
  }
  assert.fail(`${name} was not reachable in the active panel`);
}

async function quitVC() {
  send('\x1b[21~');
  await until('the quit confirmation', (text) =>
    text.includes('Do you want to quit the Volkov Commander?') && text.includes('Yes'));
  send('\r');
  await until('the JavaScript exit hook', () => exitCalls > 0);
  assert.equal(exitCalls, 1, 'the exit hook fires exactly once');
  assert.equal(exitCode, 0, 'VC exits successfully');
}

async function checkFetchFailures() {
  const markerPath = '/home/vc/KEEP.TXT';
  vc.FS.writeFile(markerPath, 'the same MEMFS survives all failed downloads');
  const errors = () => {
    try {
      return (vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' })
        .match(/DOS command error 5/g) || []).length;
    } catch { return 0; }
  };
  for (const failure of ['rejected fetch', 'HTTP 503', 'HTTP 404 (stale deploy)', 'malformed wasm']) {
    stage = `fetch failure: ${failure}`;
    const before = errors();
    expectedFetches.push('gwbasic.wasm');
    send('gwbasic\r');
    await releaseProgramFetch('gwbasic.wasm');
    await until(`DOS access-denied error and restored VC panels after ${failure}`, (text) =>
      errors() > before && isPanel(text));
    // VC normally restores its panels immediately. Its saved user screen
    // retains the ordinary DOS diagnostic, reachable with the real Ctrl-O.
    send('\x0f');
    await until(`the saved user screen contains the DOS error after ${failure}`, (text) =>
      !hasPanels(text) && text.includes('Access denied'));
    send('\x0f');
    await until(`VC panels after showing the ${failure} DOS error`, isPanel);
    assert.equal(exitCalls, 0, 'failed EXEC must not exit VC');
    assert.equal(vc.FS.readFile(markerPath, { encoding: 'utf8' }),
      'the same MEMFS survives all failed downloads');
    assertFetches();
    console.log(`PASS failure: ${failure} returns DOS error 5 and VC restores its panels`);
  }
  assert.equal(moduleFetches[1].bodyReads, 0, 'an HTTP error must be rejected before reading its body');
  assert.equal(moduleFetches[2].bodyReads, 0, 'a stale-tab 404 must be rejected without linking a newer module');

  stage = 'fetch failure: VC remains usable';
  await selectFile('README.TXT');
  const firstLine = vc.FS.readFile('/home/vc/README.TXT', { encoding: 'utf8' }).split(/\r?\n/)[0];
  send('\x1bOR');
  await until('working F3 after all failed EXECs', (text) => text.includes(firstLine));
  send('\x1b');
  await until('VC panels after the post-failure viewer', isPanel);

  stage = 'fetch failure: successful retry';
  expectedFetches.push('gwbasic.wasm');
  send('gwbasic\r');
  await releaseProgramFetch('gwbasic.wasm');
  await until('GW-BASIC starts after failed and malformed downloads', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  send('PRINT 2+2\r');
  await until('the recovered BASIC executes arithmetic', (text) => /^\s*4\s*$/m.test(text));
  send('SYSTEM\r');
  await until('VC panels after the recovered BASIC', isPanel);

  stage = 'fetch failure: cached rerun';
  send('gwbasic\r');
  await until('the successfully retried module runs from cache', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  assertFetches();
  assert.equal(moduleFetches.length, 5, 'four failed attempts and one successful fetch, no cached fetch');
  assert.deepEqual(moduleFetches.map(({ bodyReads }) => bodyReads), [0, 0, 0, 1, 1],
    'the production fetch path reads only HTTP-success bodies, including its successful retry');
  send('SYSTEM\r');
  await until('VC panels after the cached recovered module', isPanel);
  await quitVC();
  console.log('web fetch failure: rejected/HTTP-503/stale-deploy-404/malformed downloads, DOS errors, working F3, retry and cache passed');
}

function dosErrors(code) {
  try {
    const log = vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' });
    return (log.match(new RegExp(`DOS command error ${code}\\b`, 'g')) || []).length;
  } catch { return 0; }
}

async function checkUsableAfterFailedLoad(message) {
  send('\x0f');
  await until(`the saved DOS diagnostic: ${message}`, (text) =>
    !hasPanels(text) && text.includes(message));
  send('\x0f');
  await until('VC panels after the DOS diagnostic', isPanel);
  await selectFile('README.TXT');
  const firstLine = vc.FS.readFile('/home/vc/README.TXT', { encoding: 'utf8' }).split(/\r?\n/)[0];
  send('\x1bOR');
  await until('F3 still works after the failed load', (text) => text.includes(firstLine));
  send('\x1b');
  await until('panels after the post-failure F3 viewer', isPanel);
  assert.equal(exitCalls, 0, 'a failed load must not end VC');
  assert.deepEqual(vc.FS.readdir('/var/vc').filter((name) => name.endsWith('.wasm')), [],
    'failed loads leave no staged module files');
}

async function checkSuccessfulRetry() {
  expectedFetches.push('gwbasic.wasm');
  send('gwbasic\r');
  await releaseProgramFetch('gwbasic.wasm');
  await until('BASIC starts on a later EXEC', (text) => text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  send('PRINT 6*7\r');
  await until('the retried BASIC executes correctly', (text) => /^\s*42\s*$/m.test(text));
  send('SYSTEM\r');
  await until('panels after the successfully retried BASIC', isPanel);
  send('gwbasic\r');
  await until('the retried module is cached', (text) => text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  assertFetches();
  send('SYSTEM\r');
  await until('panels after the cached retry', isPanel);
}

async function checkStalledDownloads() {
  for (const [failure, configured] of [
    ['stalled response', 100], ['stalled response body', 100],
    ['default deadline', undefined], ['capped deadline', 60000],
  ]) {
    stage = failure;
    vc.vcProgramFetchTimeoutMs = configured;
    const before = dosErrors(5);
    const deadlinesBefore = programDeadlines.length;
    expectedFetches.push('gwbasic.wasm');
    send('gwbasic\r');
    await releaseProgramFetch('gwbasic.wasm');
    await until(`${failure} aborts with DOS error 5 and restores panels`, (text) =>
      dosErrors(5) > before && isPanel(text));
    assert.equal(moduleFetches.at(-1).signal?.aborted, true, 'the timeout aborts the actual transport');
    await checkUsableAfterFailedLoad('Access denied');
    if (configured !== 100) {
      assert.deepEqual(programDeadlines.slice(deadlinesBefore), [30000],
        'the production deadline defaults to and is capped at exactly 30 seconds');
    }
    const request = moduleFetches.at(-1);
    if (request.finishLate) {
      const bytes = await programBytes(new URL(request.url));
      request.finishLate(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
      await new Promise((done) => setImmediate(done));
      assert.deepEqual(vc.FS.readdir('/var/vc').filter((name) => name.endsWith('.wasm')), [],
        'a response body arriving after timeout cannot stage or link a module');
      assert.equal(dosErrors(5), before + 1, 'late completion cannot resume the failed EXEC again');
      assert.ok(isPanel(screen));
    }
    console.log(`PASS timeout: ${failure}; aborted transport, DOS error 5, F3 usable`);
  }
  assert.deepEqual(moduleFetches.map(({ bodyReads }) => bodyReads), [0, 1, 0, 0]);
  vc.vcProgramFetchTimeoutMs = 100;
  await checkSuccessfulRetry();
  await quitVC();
  console.log('web fetch timeout: response/body stalls, 30s default/cap, late bytes, DOS errors, F3, retry and cache passed');
}

async function checkMemoryLimit() {
  stage = 'memory-limited EXEC';
  const markerPath = '/home/vc/KEEP.TXT';
  vc.FS.writeFile(markerPath, 'files survive exhausted wasm memory');
  // A real fixed-capacity WebAssembly.Memory, not a mocked availability
  // counter. Consume its free heap, retaining about 1-2 MiB for VC to recover.
  const reservations = [];
  for (;;) {
    const pointer = wasmExports.malloc(1024 * 1024);
    if (!pointer) break;
    reservations.push(pointer);
  }
  assert.ok(reservations.length > 1, 'the test consumes real heap allocations');
  wasmExports.free(reservations.pop());
  for (let attempt = 0; attempt < 2; attempt++) {
    const before = dosErrors(8);
    expectedFetches.push('gwbasic.wasm');
    send('gwbasic\r');
    await releaseProgramFetch('gwbasic.wasm');
    await until('insufficient heap gives DOS error 8 and restores VC', (text) =>
      dosErrors(8) > before && isPanel(text));
    assert.equal(vc.FS.readFile(markerPath, { encoding: 'utf8' }), 'files survive exhausted wasm memory');
    await checkUsableAfterFailedLoad('Not enough memory');
    console.log(`PASS memory limit: attempt ${attempt + 1} returns DOS error 8, preserves files and working F3`);
  }
  for (const pointer of reservations) wasmExports.free(pointer);

  // Enough aggregate free space is not enough for dylink's raw-file copy.
  // Make many separated holes in the same fixed memory to exercise the
  // contiguous-allocation check rather than only the free-byte threshold.
  stage = 'fragmented-memory EXEC';
  const fragments = [];
  const fragmentSize = 128 * 1024;
  for (;;) {
    const pointer = wasmExports.malloc(fragmentSize);
    if (!pointer) break;
    fragments.push(pointer);
  }
  let freed = 0;
  for (let index = 0; index < fragments.length; index += 2) {
    wasmExports.free(fragments[index]);
    fragments[index] = 0;
    freed += fragmentSize;
  }
  const moduleSize = readFileSync(resolve(dirname(modulePath), programFiles.get('gwbasic.wasm'))).length;
  assert.ok(freed >= moduleSize * 2 + 64 * 1024, 'fragmented free bytes exceed the whole module allowance');
  const before = dosErrors(8);
  expectedFetches.push('gwbasic.wasm');
  send('gwbasic\r');
  await releaseProgramFetch('gwbasic.wasm');
  await until('fragmented heap is refused with DOS error 8', (text) => dosErrors(8) > before && isPanel(text));
  await checkUsableAfterFailedLoad('Not enough memory');
  for (const pointer of fragments) if (pointer) wasmExports.free(pointer);
  console.log(`PASS memory fragmentation: ${freed} aggregate free bytes in 128 KiB holes; DOS error 8 and F3 usable`);
  await checkSuccessfulRetry();
  assert.equal(vc.HEAPU8.byteLength, startupHeapBytes, 'the constrained memory never grew');
  await quitVC();
  console.log(`web memory limit: ${startupHeapBytes} fixed bytes, exhausted/fragmented refusals, retry and cache passed`);
}

function fail(error) {
  let log = '';
  try {
    screen = vc.FS.readFile(screenPath, { encoding: 'utf8' });
    log = vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' });
  } catch { /* A startup failure may precede the screen/log. */ }
  console.error(`FAIL [${stage}]: ${error?.stack || error}\n${screen}\n--- VC log ---\n${log}`);
  console.error(`Speaker events in current stage: ${JSON.stringify(speakerEvents)}`);
  if (sourcePanel) {
    const view = sourcePanel.view;
    console.error(`Source view: ${JSON.stringify(view && {
      status: view.status, message: view.message, current: view.current,
      callers: view.callers, recentLines: view.recent?.length,
    })}`);
  }
  exitAfterOutput(1);
}

// An Asyncify rewind runs from a timer, outside the async test's try/catch.
// Preserve the same stage/screen/log diagnostics for guest runtime traps.
process.on('uncaughtException', fail);
process.on('unhandledRejection', fail);

try {
  if (fetchFailure || fetchTimeout) {
    // Exercise the browser's real fetch/status/arrayBuffer branch entirely
    // in memory. No listener, HTTP server, or network request is involved.
    globalThis.fetch = async (url, options) => {
      const { request, resource, attempt } = await programRequest(url);
      if (fetchTimeout) {
        request.signal = options?.signal;
        // Ignore cancellation deliberately: Promise.race must bound both
        // fetch() and arrayBuffer(), even with an uncooperative transport.
        if (attempt === 1 || attempt === 3 || attempt === 4) return new Promise(() => {});
        return {
          ok: true,
          status: 200,
          async arrayBuffer() {
            request.bodyReads++;
            if (attempt === 2) return new Promise((resolveLate) => { request.finishLate = resolveLate; });
            const bytes = await programBytes(resource);
            return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
          },
        };
      }
      if (attempt === 1) throw new Error('simulated offline module fetch');
      return {
        ok: attempt !== 2 && attempt !== 3,
        status: attempt === 2 ? 503 : attempt === 3 ? 404 : 200,
        async arrayBuffer() {
          request.bodyReads++;
          // The 503 body is valid wasm if accidentally read, so ignoring
          // response.ok would start BASIC instead of returning a DOS error.
          const bytes = attempt === 4 ? new Uint8Array([0, 1, 2, 3]) : await programBytes(resource);
          return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
        },
      };
    };
  }
  if (!fetchFailure && !fetchTimeout && !memoryLimit) await installSourcePanel();
  const wasmBinary = await readFile(new URL('./vc.wasm', moduleURL));
  await createVC({
    wasmBinary,
    // Match main's declared minimum, but prohibit all growth. The pressure
    // and fragmentation scenarios then consume real allocations in this cap.
    ...(memoryLimit ? { wasmMemory: new WebAssembly.Memory({ initial: 768, maximum: 768 }) } : {}),
    instantiateWasm(imports, receiveInstance) {
      const compiled = new WebAssembly.Module(wasmBinary);
      const instance = new WebAssembly.Instance(compiled, imports);
      wasmExports = instance.exports;
      return receiveInstance(instance, compiled);
    },
    locateFile: (name) => new URL(`${name}?v=${buildHash}`, moduleURL).href,
    vcFetchProgram: fetchFailure || fetchTimeout ? undefined : fetchProgramBytes,
    ...(fetchTimeout ? { vcProgramFetchTimeoutMs: 100 } : {}),
    preRun: [(module) => {
      vc = module;
      module.ENV.VC_SCREEN_DUMP = screenPath;
      module.ENV.VC_FRAME_DUMP = framePath;
    }],
    vcReadInput(capacity) {
      inputReads++;
      observe();
      const bytes = pending.slice(0, capacity);
      pending = pending.subarray(bytes.length);
      return bytes;
    },
    vcOutput() {
      outputCalls++;
      observe();
    },
    vcExit(code) {
      exitCode = code;
      exitCalls++;
      observe();
    },
    vcSpeaker(hz) {
      speakerEvents.push(hz);
      observe();
    },
    vcGraphics(frame) {
      graphics = frame;
      if (frame) graphicsFrames++;
      else textTransitions++;
      observe();
    },
    vcSourceSnapshot(snapshot) { sourceSnapshot = snapshot; },
    vcSourceResolved(location) { sourceResolved = location; },
    print: (text) => console.log(`[VC stdout] ${text}`),
    printErr: (text) => console.error(`[VC stderr] ${text}`),
  });

  await until('10Quit and README in the H: listing', (text) =>
    isPanel(text) && text.includes('H:\\'));
  assertFetches();
  assert.equal(moduleFetches.length, 0, 'VC reaches its first screen before any program module is fetched');
  assert.equal(sourceFetches.length, 0, 'no source index, maps or source text load before the first VC screen');
  startupHeapBytes = vc.HEAPU8.byteLength;
  startupHeapTop = wasmExports.sbrk(0);
  assert.equal(vc.FS.cwd(), '/home/vc');
  assert.ok(!screen.includes('.config') && !screen.includes('.cache'),
    'VC keeps its settings and log off the H: demo drive');
  assert.ok(vc.FS.stat('/home/vc/GWBASIC.EXE').size > 50000,
    'GW-BASIC is an actual MZ file on H:');
  assert.ok(!vc.FS.analyzePath('/home/vc/GAMES/NOTHING.TXT').exists);
  console.log('PASS 1: startup shows 10Quit and README on H:');

  if (fetchFailure || fetchTimeout || memoryLimit) {
    if (fetchFailure) await checkFetchFailures();
    else if (fetchTimeout) await checkStalledDownloads();
    else if (memoryLimit) await checkMemoryLimit();
    exitAfterOutput(0);
    await new Promise(() => {});
  }

  stage = 'Source VC, F9 and unchanged screen';
  const beforeSource = screen;
  let sourceView = await openSource(['VC.COM', 'VC.OVL']);
  assertVCSource(sourceView);
  assert.equal(screen, beforeSource, 'opening Source does not change VC or its key bar');
  assert.ok(sourceFetches.length > 1, 'opening Source fetches its maps and original source text lazily');
  const openedSources = sourceFetches.filter(({ url }) => new URL(url).pathname.endsWith('.txt'));
  console.log(`PASS Source first open: ${openedSources.length} original text files, ${openedSources.reduce((sum, item) => sum + item.bytes, 0)} bytes; ${sourceFetches.reduce((sum, item) => sum + item.bytes, 0)} bytes including maps/index`);
  sourceButton.dispatch('click');
  assert.equal(sourcePanel.opened, false, 'the same Source button closes the panel');
  assert.equal(sourceElement.hidden, true);
  assert.equal(screen, beforeSource);
  sourceShortcut();
  await until('Ctrl-Shift-F12 reopens Source from the cached maps', () =>
    sourcePanel.opened && sourcePanel.view?.status === 'ready');
  assertVCSource(sourcePanel.view);
  assert.equal(screen, beforeSource);
  const drawsBeforeF9 = sourceDraws;
  send('\x1b[20~');
  await until('F9 shows the real VC menu bar', (text) =>
    /Left\s+Files\s+Commands\s+Options\s+Right/.test(text.split('\n')[0]));
  // Do not invoke refresh here: the production panel must notice this key
  // and sample the caller chain after the menu itself has taken over input.
  await until('Source refresh after F9 identifies the original menu caller', () =>
    sourceDraws > drawsBeforeF9 && sourcePanel.view?.callers?.some((line) => line.path === 'asm/VCMENU.INC'));
  sourceView = sourcePanel.view;
  assertVCSource(sourceView);
  assert.ok(sourceView.callers.some((line) => /\bCALL\s+Input\b/i.test(line.text)),
    'Called from shows the original CALL that entered the F9 menu keyboard loop');
  await closeSourceWithShortcut();
  send('\x1b');
  await until('VC panels after closing the F9 menu', isPanel);
  assert.equal(screen, beforeSource, 'Escape still closes F9 and restores the exact pre-Source VC screen');
  console.log('PASS Source VC: lazy open, exact Now/Called from text, F9 auto-refresh, distinct history, unchanged screen and Escape');

  stage = 'Source physical modifiers';
  await checkSourceModifierRelease();
  console.log('PASS Source shortcut: actual page handler, physical Ctrl/Shift, both release orders, repeat suppression and unchanged VC keys');

  stage = '2 F3 viewer';
  await selectFile('README.TXT');
  const firstLine = vc.FS.readFile('/home/vc/README.TXT', { encoding: 'utf8' })
    .split(/\r?\n/)[0];
  assert.ok(firstLine.length, 'the demo README must not be empty');
  send('\x1bOR');
  await until('the README first line in F3', (text) => text.includes(firstLine));
  console.log(`PASS 2: F3 displays ${JSON.stringify(firstLine)}`);
  send('\x1b');
  await until('the panels after closing F3', isPanel);

  stage = '2b Russian F3 viewer';
  const russianBytes = vc.FS.readFile('/home/vc/ПРОЧТИ.TXT');
  const russian = new TextDecoder('ibm866').decode(russianBytes);
  assert.equal(russian,
    readFileSync(new URL('../web/README-RU.TXT', import.meta.url), 'utf8').replaceAll('\n', '\r\n'),
    'the actual MEMFS Russian README is complete CP866, not UTF-8');
  const russianFirstLine = 'Volkov Commander в вашем браузере.';
  assert.equal(russian.split('\r\n')[0], russianFirstLine);
  await selectFile('ПРОЧТИ.TXT');
  send('\x1bOR');
  await until('the Russian first line and Cyrillic text in F3', (text) =>
    text.includes(russianFirstLine) && text.includes('Это не эмулятор.'));
  console.log('PASS Russian: F3 displays H:\\ПРОЧТИ.TXT correctly from its CP866 bytes');
  send('\x1b');
  await until('the panels after the Russian viewer', isPanel);

  stage = '3 no shell';
  send('echo hi\r');
  const noShell = 'No shell in the browser, only cd works here. The Linux version runs '
    + 'commands: github.com/evoleinik/vc-linux';
  await until('the exact no-shell message on the user screen', (text) =>
    text.replaceAll('\n', '').includes(noShell));
  console.log('PASS 3: echo hi displays the exact no-shell message');
  await until('the panels after the command', isPanel);

  stage = '4 kitty Ctrl-[';
  const leftPath = screen.split('\n')[0].slice(0, 40).match(/[A-Z]:\\[^ ═╤╗]*/)?.[0];
  assert.ok(leftPath, 'the left panel displays its path');
  assert.equal(commandLine(screen).trimEnd(), 'H:\\>', 'command line starts empty');
  send('\x1b[91;5u');
  await until('the left panel path on the command line', (text) =>
    commandLine(text).includes(`H:\\>${leftPath}`));
  console.log(`PASS 4: kitty Ctrl-[ puts ${leftPath} on the command line`);
  // Release Ctrl before F10, then clear the inserted path without executing it.
  send('\x1b[57442;1:3u\x1b');
  await until('an empty command line', (text) =>
    commandLine(text).trimEnd() === 'H:\\>');

  stage = '5 GW-BASIC banner';
  expectedFetches.push('gwbasic.wasm');
  send('gwbasic\r');
  await releaseProgramFetch('gwbasic.wasm');
  await until('GW-BASIC banner and Ok from its own code', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  console.log('PASS 5: DOS PATH/EXEC starts H:\\GWBASIC.EXE and shows Ok');

  stage = 'Source GW-BASIC and unchanged input';
  sourceView = await openSource('GWBASIC.EXE');
  assertSourceLine(sourceView.current);
  assert.match(sourceView.current.path, /^third_party\/gwbasic\//, 'BASIC maps to its own vendored assembly');
  await closeSourceWithShortcut();
  console.log('PASS Source BASIC: the real child maps to its assembly and closing preserves its screen/input');

  stage = '6 BASIC arithmetic';
  send('PRINT 2+2\r');
  await until('PRINT 2+2 to print 4', (text) => /^\s*4\s*$/m.test(text));
  console.log('PASS 6: GW-BASIC PRINT 2+2 prints 4');

  async function sound(command, frequencies, label) {
    speakerEvents.length = 0;
    send(`${command}\r`);
    await until(`${label} through the speaker hook`, () => {
      const audible = speakerEvents.filter((hz) => hz > 0);
      return frequencies.every((expected) => audible.some((hz) => Math.abs(hz - expected) < 2))
        && speakerEvents.at(-1) === 0;
    });
    assert.ok(speakerEvents.every((hz) => Number.isFinite(hz) && hz >= 0), 'valid speaker frequency');
  }
  stage = '7 BASIC BEEP';
  await sound('BEEP', [800], 'BEEP (PIT divisor 1491)');
  console.log('PASS 7: BEEP reaches the speaker hook at ~800 Hz and stops');
  stage = '8 BASIC SOUND';
  await sound('SOUND 440,18', [440], 'SOUND 440,18');
  console.log('PASS 8: SOUND 440,18 reaches the speaker hook at ~440 Hz');
  stage = '9 BASIC PLAY';
  // This 1983 fork defaults to OCTAVE=4, and divides NOTTAB by 2^(6-4).
  // The resulting PIT divisors are 1140, 1015, 905 (not modern middle C).
  await sound('PLAY "CDE"', [1047, 1175, 1319], 'PLAY CDE');
  console.log('PASS 9: PLAY CDE reaches the speaker hook at ~1047/1175/1319 Hz');

  stage = '10 BASIC SYSTEM';
  send('SYSTEM\r');
  await until('VC panels after SYSTEM', isPanel);
  console.log('PASS 10: SYSTEM returns to redrawn VC panels');

  stage = '11 untranslated BASIC call';
  send('gwbasic\r');
  await until('GW-BASIC ready before an untranslated CALL', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  speakerEvents.length = 0;
  for (const command of ['OUT &H43,&HB6', 'OUT &H42,0', 'OUT &H42,4', 'OUT &H61,3']) {
    send(`${command}\r`);
    await until(`Ok after ${command}`, (text) => {
      const at = text.lastIndexOf(command);
      return at >= 0 && /^Ok\s*$/m.test(text.slice(at));
    });
  }
  await until('the child leaving its speaker on', () => speakerEvents.at(-1) > 0);
  // CALL takes an address variable in this GW-BASIC source version.
  send('DEF SEG=0: A=0: CALL A\r');
  await until('the child-only no-translation message', () => sawFaultMessage);
  await until('VC panels after stopping only GW-BASIC', isPanel);
  assert.equal(exitCalls, 0, 'an untranslated child must not exit VC');
  assert.equal(speakerEvents.at(-1), 0, 'aborting a child stops its speaker');
  send('gwbasic\r');
  await until('GW-BASIC can run again after an untranslated CALL', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  await sound('SOUND 440,1', [440], 'sound after child recovery');
  send('SYSTEM\r');
  await until('VC panels after the recovered child exits', isPanel);
  assertFetches();
  console.log('PASS 11: untranslated CALL stops only GW-BASIC; VC and a second BASIC still work');

  stage = '12 bootLogo command';
  assert.equal(vc.FS.stat('/home/vc/BOOTLOGO.COM').size, 503, 'BOOTLOGO.COM is the original NASM COM file on H:');
  assert.ok(!vc.FS.analyzePath('/home/vc/LOGO.COM').exists, 'the old LOGO.COM alias is not installed');
  assert.ok(!vc.FS.analyzePath('/home/vc/LOGO.TXT').exists, 'the guide is installed under its own name');
  assert.match(vc.FS.readFile('/home/vc/BOOTLOGO.TXT', { encoding: 'utf8' }), /TO FLOWER REPEAT 4 \[PETAL LT 50\] END/);
  expectedFetches.push('bootlogo.wasm');
  send('bootlogo\r');
  await releaseProgramFetch('bootlogo.wasm');
  await until('bootLogo prompt in the graphics canvas', () =>
    graphics?.mode === 4 && graphicsText(graphics).split('\n')[21].trimEnd() === '>');
  assert.equal(graphics.width, 320);
  assert.equal(graphics.height, 200);
  assert.equal(graphics.pixels.length, 64000);
  assert.deepEqual([...graphics.palette], [0, 0, 0, 85, 255, 255, 255, 85, 255, 255, 255, 255]);
  console.log('PASS 12: bootlogo starts the real H:\\BOOTLOGO.COM with a CGA canvas frame');

  stage = 'Source bootLogo';
  sourceView = await openSource('LOGO.COM');
  assertSourceLine(sourceView.current);
  assert.match(sourceView.current.path, /^third_party\/bootlogo\//, 'bootLogo maps to its own NASM source');
  await closeSourceWithShortcut();
  console.log('PASS Source bootLogo: the running graphics program maps to its original assembly');

  stage = '13 bootLogo square pixels and dumps';
  send('REPEAT 4 [FD 50 RT 90]\r');
  // Independent from the framebuffer decoder: bootLogo starts at 160,100
  // with 7 fractional bits and exact cardinal steps of 128 in its sin table.
  const corners = [[160, 100], [160, 50], [210, 50], [210, 100]];
  await until('all four exact square corners in the canvas hook', () =>
    corners.every(([x, y]) => pixel(x, y) === 3));
  for (let x = 160; x <= 210; x++) assert.equal(pixel(x, 50), 3, `square top at ${x},50`);
  for (let y = 50; y <= 100; y++) assert.equal(pixel(210, y), 3, `square right at 210,${y}`);
  assert.equal(pixel(185, 75), 0, 'square interior remains background');
  assert.ok(graphicsFrames > 1, 'the canvas receives drawing updates');
  const pgm = vc.FS.readFile(framePath);
  const pgmHeader = encoder.encode('P5\n320 200\n3\n');
  assert.deepEqual(pgm.slice(0, pgmHeader.length), pgmHeader);
  assert.deepEqual(pgm.slice(pgmHeader.length), graphics.pixels, 'PGM stores every unscaled raw pixel');
  assert.match(screen, /[\u2801-\u28ff]/, 'graphics screen dump contains braille');
  assert.equal(screen.trimEnd().split('\n').length, 25);
  console.log('PASS 13: square corners/edges reach the canvas; exact PGM and braille dumps agree');

  stage = '14 bootLogo QUIT and VC text restoration';
  const transitionsBeforeQuit = textTransitions;
  send('QUIT\r');
  await until('VC original graphics-return confirmation', () =>
    graphicsText(graphics).includes('Press ENTER to return'));
  send('\r');
  await until('VC panels with the canvas hidden after QUIT and Enter', (text) =>
    isPanel(text) && graphics === null && textTransitions > transitionsBeforeQuit);
  console.log('PASS 14: QUIT, then VC\'s own Enter confirmation restores text panels and hides the canvas');

  stage = '15 GW-BASIC CGA pixels';
  send('gwbasic\r');
  await until('GW-BASIC ready for graphics', (text) => text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  await basicLine('SCREEN 1: LINE (10,10)-(100,10): CIRCLE (160,100),40');
  await until('GW-BASIC direct-video LINE and CIRCLE pixels', () =>
    pixel(100, 10) === 3 && pixel(120, 100) === 3 && pixel(200, 100) === 3);
  for (let x = 10; x <= 100; ++x) assert.equal(pixel(x, 10), 3, `BASIC LINE at ${x},10`);
  assert.equal(pixel(160, 100), 0);
  await basicLine('PSET (17,151),2: DRAW "BM250,90 R20 D20 L20 U20"');
  await until('GW-BASIC PSET and DRAW pixels', () => pixel(17, 151) === 2 && pixel(270, 110) === 2);
  await basicLine('SCREEN 0');
  await until('SCREEN 0 returns the canvas to xterm', (text) => graphics === null && /^Ok\s*$/m.test(text));
  send('SYSTEM\r');
  await until('panels after BASIC graphics', isPanel);
  console.log('PASS 15: BASIC SCREEN 1/LINE/CIRCLE/PSET/DRAW pixels and SCREEN 0 text restoration');

  stage = '16 shipped SPIRAL.BAS';
  const spiral = vc.FS.readFile('/home/vc/GAMES/SPIRAL.BAS', { encoding: 'utf8' });
  assert.ok(spiral.includes('SCREEN 1') && spiral.includes('DRAW '));
  assert.ok(!spiral.replaceAll('\r\n', '').includes('\n'), 'SPIRAL.BAS is DOS CRLF text');
  // This FCB-based BASIC takes an 8.3 name, not a subdirectory path. Use
  // the documented workflow: open GAMES, select the file, press Enter.
  send('cd GAMES\r');
  await until('the GAMES panel', (text) => hasPanels(text) && commandLine(text).trimEnd() === 'H:\\GAMES>');
  await selectFile('SPIRAL.BAS');
  send('\r');
  await until('the shipped DRAW spiral to complete', () =>
    graphicsText(graphics).includes('PRESS A KEY TO RETURN TO TEXT'));
  assert.ok(graphics.pixels.slice(0, 320 * 160).filter((p) => p === 3).length > 3000);
  send(' ');
  await until('spiral key returns to text', (text) =>
    graphics === null && text.includes('SPIRAL DONE - TYPE SYSTEM FOR VC'));
  send('SYSTEM\r');
  await until('panels after SPIRAL', (text) => hasPanels(text) && graphics === null);
  send('cd ..\r');
  await until('the H: root panel after SPIRAL', isPanel);
  console.log('PASS 16: shipped SPIRAL.BAS draws and its key returns to text');

  stage = '17 bootLogo Ctrl-Break during a nonpolling loop';
  const keptFile = '/home/vc/KEEP.TXT';
  vc.FS.writeFile(keptFile, 'survives Ctrl-Break without a page reload');
  for (const [shortcut, key] of [['Ctrl-Pause', '\x1b[57362;5u'], ['Ctrl-Shift-B', '\x1b[98;6u']]) {
    send('bootlogo\r');
    await until(`bootLogo prompt before ${shortcut}`, () =>
      graphics?.mode === 4 && graphicsText(graphics).split('\n')[21].trimEnd() === '>');
    const command = 'REPEAT 0 [REPEAT 0 [FD 1]]';
    send(command);
    await until('the complete nested-loop input echo', () =>
      graphicsText(graphics).split('\n')[21].startsWith('>' + command));
    const framesBeforeLoop = graphicsFrames;
    send('\r');
    await until('a new canvas frame while bootLogo executes without keyboard polls', () =>
      graphicsFrames > framesBeforeLoop && pixel(160, 40) === 3);
    assert.notEqual(graphicsText(graphics).split('\n')[21].trimEnd(), '>',
      'the four-billion-point command must still be running when Ctrl-Break is sent');
    // A queued ordinary key must not suppress the dispatcher's browser
    // yield: bootLogo will not consume that key while this loop is running.
    const readsBeforeTypeahead = inputReads;
    send('x');
    await until('the busy guest keeps polling with unread keyboard typeahead', () =>
      inputReads > readsBeforeTypeahead + 1 && pending.length === 0);
    send(key);
    await until(`VC return confirmation after ${shortcut}`, () =>
      graphicsText(graphics).includes('Press ENTER to return'));
    send('\x1b[57442;1:3u\x1b[57441;1:3u\r');
    await until(`VC panels and a hidden canvas after ${shortcut}`, (text) =>
      isPanel(text) && graphics === null);
    assert.equal(exitCalls, 0, 'Ctrl-Break ends only the child');
    assert.equal(vc.FS.readFile(keptFile, { encoding: 'utf8' }),
      'survives Ctrl-Break without a page reload', 'H: files survive child cancellation');
  }
  assertFetches();
  console.log('PASS 17: both Ctrl-Break shortcuts stop a busy bootLogo, with live canvas updates and H: preserved');

  stage = '18 GW-BASIC retains its own Ctrl-Break handler';
  send('gwbasic\r');
  await until('GW-BASIC ready for its hooked Ctrl-Break', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  await basicLine('10 PRINT "BASIC LOOP RUNNING":GOTO 10');
  send('RUN\r');
  await until('the BASIC loop is executing', (text) => /^BASIC LOOP RUNNING\s*$/m.test(text));
  send('\x1b[57362;5u');
  await until('BASIC handles INT 1Bh itself', (text) => text.includes('Break in 10'));
  send('\x1b[57442;1:3uSYSTEM\r');
  await until('VC panels after the interrupted BASIC exits normally', isPanel);
  console.log('PASS 18: GW-BASIC still handles Ctrl-Break with Break in 10 and SYSTEM');

  stage = '19 Rogue installation and typed launch';
  const rogueImage = vc.FS.readFile('/home/vc/GAMES/ROGUE.EXE');
  assert.equal(String.fromCharCode(...rogueImage.slice(0, 2)), 'MZ',
    'ROGUE.EXE is a real compiled DOS image in H:\\GAMES');
  assert.ok(rogueImage.length > 50000, 'the game must contain the linked Rogue/PDCurses program');
  assert.ok(!vc.FS.analyzePath('/home/vc/ROGUE.EXE').exists,
    'DOS PATH finds H:\\GAMES without a second root copy');
  assert.match(vc.FS.readFile('/home/vc/GAMES/ROGUELIC.TXT', { encoding: 'utf8' }),
    /Michael Toy, Ken Arnold and Glenn Wichman/);
  assert.match(vc.FS.readFile('/home/vc/GAMES/PDCLIC.TXT', { encoding: 'utf8' }), /public domain/);
  expectedFetches.push('rogue.wasm');
  send('rogue\r');
  await releaseProgramFetch('rogue.wasm');
  await until('Rogue dungeon and Level: 1 Gold: Hp: status', (text) =>
    isRogue(text) && rogueStatus.exec(text)[1] === '1');
  assert.equal(graphics, null, 'PDCurses uses the real DOS text screen');
  console.log('PASS 19: rogue resolves the real H:\\GAMES\\ROGUE.EXE and shows a level-1 dungeon');

  stage = 'Source Rogue function map';
  sourceView = await openSource('ROGUE.EXE');
  assert.equal(typeof sourceView.current.function, 'string', 'Rogue reports a compiled C function');
  assert.ok(sourceView.current.function.length > 0);
  assert.ok(Number.isInteger(sourceView.current.offset) && sourceView.current.offset >= 0,
    'Rogue reports the real non-negative offset into its C function');
  assert.equal(sourceView.current.label, 'C function (map)', 'Rogue is explicitly labelled as a map-file C function');
  assert.ok(sourceView.current.line == null, 'Rogue must not invent C line numbers');
  assert.ok(sourceView.current.path == null, 'the linker map does not invent a C source filename');
  const rogueMap = readFileSync(new URL('../build/rogue/ROGUE.MAP', import.meta.url), 'utf8');
  const symbols = [...rogueMap.matchAll(/^([0-9a-f]{4}):([0-9a-f]{4})[*+]?\s+(\S+)\s*$/gim)];
  assert.equal(sourceView.current.address, sourceView.snapshot.current.offset,
    'Rogue function lookup uses the real current canonical address');
  assert.ok(symbols.some(([, segment, offset, name]) => name === sourceView.current.function
    && parseInt(segment, 16) * 16 + parseInt(offset, 16) + sourceView.current.offset === sourceView.current.address),
    'the reported Rogue function name and offset agree with the original linker map');
  assert.ok(sourceElement.textContent.includes(sourceView.current.function));
  assert.match(sourceElement.textContent, /C function[^\n]*map/, 'the visible panel labels the C function as map-derived');
  await closeSourceWithShortcut();
  console.log('PASS Source Rogue: truthful C function name/offset and map label, no invented source line');

  stage = '20 Rogue movement';
  const [oldRow, oldColumn] = roguePlayers(screen)[0];
  const rogueLines = screen.split('\n');
  const moves = [['h', 0, -1], ['j', 1, 0], ['k', -1, 0], ['l', 0, 1],
    ['y', -1, -1], ['u', -1, 1], ['b', 1, -1], ['n', 1, 1]];
  const movement = ['.', '*%:!?)=/]'].flatMap((floor) =>
    moves.filter(([, dy, dx]) => floor.includes(rogueLines[oldRow + dy]?.[oldColumn + dx])))[0];
  assert.ok(movement, 'the lit first-level room must have an adjacent unoccupied floor');
  const [moveKey, dy, dx] = movement;
  send(moveKey);
  // Some traps emit two messages before updating @. Handle their visible
  // prompt while waiting, without allowing a dropped movement key to pass.
  for (let messages = 0; messages <= 8; ++messages) {
    await until('Rogue movement changes @ or asks --More--', (text) => text.includes('--More--')
      || isRogue(text) && roguePlayers(text).some(([row, col]) => row !== oldRow || col !== oldColumn));
    if (!screen.includes('--More--')) break;
    assert.ok(messages < 8, 'Rogue movement produces at most eight continuation prompts');
    const previous = screen;
    send(' ');
    await until('Rogue acknowledges its visible --More-- prompt', (text) => text !== previous);
  }
  assert.ok(!screen.includes('--More--'), 'Rogue is ready for the next command');
  // Level 1 can contain a hidden teleport trap. T_TELEP in move.c silently
  // relocates @ and draws ^ at the selected cell; @ must still change.
  const revealedTeleport = rogueLines[oldRow + dy][oldColumn + dx] !== '^'
    && screen.split('\n')[oldRow + dy][oldColumn + dx] === '^'
    && roguePlayers(screen).some(([row, col]) => row !== oldRow || col !== oldColumn);
  if (!(screen.toLowerCase().includes('you fell into a trap!') && rogueStatus.exec(screen)[1] !== '1'
        || revealedTeleport))
    assert.deepEqual(roguePlayers(screen), [[oldRow + dy, oldColumn + dx]],
      'an ordinary movement key changes @ by exactly one requested step');
  const savedRogueState = rogueState(screen);
  console.log('PASS 20: a real movement key moves @ in Rogue');

  stage = '21 Rogue save and restore';
  send('S');
  await until('Rogue default save confirmation', (text) => text.toLowerCase().includes('save file (rogue.sav)?'));
  send('y');
  await until('VC panels after Rogue saves', isPanel);
  assert.ok(vc.FS.stat('/home/vc/rogue.sav').size > 4096, 'S writes game state to the current H: directory');
  assert.ok(vc.FS.analyzePath('/home/vc/rogue.scr').exists, 'scores use the current H: directory too');
  assert.ok(!vc.FS.analyzePath('/home/vc/GAMES/rogue.sav').exists,
    'the save directory is not the executable directory');
  send('rogue\r');
  await until('plain rogue restores the same dungeon, @ and status', (text) =>
    isRogue(text) && rogueState(text) === savedRogueState);
  assert.ok(!vc.FS.analyzePath('/home/vc/rogue.sav').exists, 'successful restore consumes the save');
  console.log('PASS 21: S/y writes the current-directory save and rogue restores the exact game');

  stage = '22 Rogue Q/y returns to VC';
  await quitRogue();
  await until('H: root panels after Rogue quit', isPanel);
  console.log('PASS 22: Q then y ends Rogue and redraws VC panels');

  stage = '23 Enter on ROGUE.EXE';
  send('cd GAMES\r');
  await until('GAMES panel for Rogue Enter launch', (text) =>
    hasPanels(text) && commandLine(text).trimEnd() === 'H:\\GAMES>');
  await selectFile('ROGUE.EXE');
  send('\r');
  await until('Enter on ROGUE.EXE starts a fresh dungeon', (text) =>
    isRogue(text) && rogueStatus.exec(text)[1] === '1');
  await quitRogue();
  send('cd ..\r');
  await until('H: root panel after Rogue Enter launch', isPanel);
  assertFetches();
  console.log('PASS 23: Enter on H:\\GAMES\\ROGUE.EXE plays and returns to VC');

  stage = '24 F4 VZ edit, save, and quit';
  assert.ok(vc.FS.analyzePath('/home/vc/VZ.COM').exists,
    'the translated VZ.COM must be installed on H: before F4 can edit');
  assert.deepEqual(vc.FS.readFile('/home/vc/VZ.COM'),
    new Uint8Array(readFileSync(new URL('../third_party/vzeditor/VZ-IBM/US/VZUS.COM', import.meta.url))),
    'VZ.COM is the real byte-identical US DOS file on H:');
  for (const [installed, source] of [['VZ.DEF', 'VZIBM.DEF'], ['VZFLE.DEF', 'VZFLE.DEF'],
    ['HELPE.DEF', 'HELPE.DEF']]) {
    assert.ok(vc.FS.analyzePath(`/home/vc/${installed}`).exists, `${installed} must be installed on H:`);
    const definition = readFileSync(new URL(`../third_party/vzeditor/VZ-IBM/${source}`, import.meta.url));
    if (installed === 'VZ.DEF') {
      const option = definition.indexOf('\r\nEb-');
      assert.ok(option >= 0, 'the vendored default is unchanged');
      definition[option + 4] = '+'.charCodeAt(0);
    }
    assert.deepEqual(vc.FS.readFile(`/home/vc/${installed}`),
      new Uint8Array(definition),
      `${installed} contains the English VZ definitions with backups enabled by default`);
  }
  await selectFile('README.TXT');
  const originalReadme = vc.FS.readFile('/home/vc/README.TXT', { encoding: 'utf8' });
  expectedFetches.push('vz.wasm');
  send('\x1bOS');
  await releaseProgramFetch('vz.wasm');
  await until('VZ showing the original README first line after F4', (text) =>
    !hasPanels(text) && text.includes(firstLine) && text.includes('File'));
  stage = 'Source VZ';
  sourceView = await openSource('VZ.COM');
  assertSourceLine(sourceView.current);
  assert.match(sourceView.current.path, /^third_party\/vzeditor\//, 'VZ maps to its vendored assembly');
  await closeSourceWithShortcut();
  console.log('PASS Source VZ: F4 child maps to its original assembly without changing editor input');
  stage = '24 F4 VZ edit, save, and quit';
  const editorRow = screen.split('\n').findIndex((line) => line.includes(firstLine));
  const editorLine = 'Edited with VZ in the browser.';
  for (let start = 0; start < editorLine.length; start += 8) {
    send(editorLine.slice(start, start + 8));
    await until(`VZ input echo through character ${start + 8}`, (text) =>
      text.includes(editorLine.slice(0, start + 8)));
  }
  send('\r');
  await until('the new VZ line above the untouched README first line', (text) => {
    const lines = text.split('\n');
    return lines[editorRow].includes(editorLine) && lines[editorRow + 1].includes(firstLine);
  });
  // The original US DEF binds Alt-S (also Esc, S) to Save As. Submit the
  // prefilled path; VZ itself, not the JS harness, must change MEMFS.
  send('\x1bs');
  await until('VZ English Save As dialog', (text) => text.includes('Save As:'));
  send('\r');
  const editedReadme = editorLine + '\r\n' + originalReadme + '\x1a';
  await until('VZ has saved the actual README bytes to MEMFS', (text) =>
    !text.includes('Save As:')
    && vc.FS.readFile('/home/vc/README.TXT', { encoding: 'utf8' }) === editedReadme);
  const readmeBackup = vc.FS.readdir('/home/vc').find((name) => name.toUpperCase() === 'README.BAK');
  assert.ok(readmeBackup, 'an F4 save must leave the previous README in README.BAK');
  assert.equal(vc.FS.readFile(`/home/vc/${readmeBackup}`, { encoding: 'utf8' }), originalReadme);
  send('\x1bq');
  await until('VZ English quit confirmation', (text) => text.includes('Quit from editor? (Y/N)'));
  send('Y');
  await until('VC panels return after VZ exits', isPanel);
  assert.equal(vc.FS.readFile('/home/vc/README.TXT', { encoding: 'utf8' }), editedReadme);
  assert.equal(exitCalls, 0, 'VZ exits only its own DOS process');
  console.log('PASS 24: F4 opens README in VZ; Alt-S saves MEMFS bytes with a backup and Alt-Q restores VC');

  stage = '25 typed VZ new file';
  send('vz NEW.TXT\r');
  await until('VZ asks to create the typed new filename', (text) =>
    text.includes('not found. New file? (Y/N)'));
  send('Y');
  await until('VZ new-file editing window', (text) =>
    !hasPanels(text) && text.toUpperCase().includes('NEW.TXT') && !text.includes('(Y/N)'));
  assert.ok(!vc.FS.readdir('/home/vc').some((name) => name.toUpperCase() === 'NEW.TXT'),
    'opening is not yet saving');
  send('New VZ.');
  await until('VZ echoes new-file text', (text) => text.includes('New VZ.'));
  const newRow = screen.split('\n').findIndex((line) => line.includes('New VZ.'));
  const beforeEnter = screen.split('\n')[newRow];
  send('\r');
  // With the shipped Dc+/De+ options, Enter replaces this row's EOF
  // marker with a CR marker and moves EOF to the next row. Observe that
  // change before sending Alt-S: VZ flushes queued control keys.
  await until('VZ inserts the new-file line ending', (text) =>
    text.split('\n')[newRow] !== beforeEnter);
  send('\x1bs');
  await until('Save As for NEW.TXT', (text) => text.includes('Save As:'));
  send('\r');
  // Dp+ in the original DEF lowercases VZ's path on the case-sensitive
  // host filesystem; DOS still resolves the typed NEW.TXT to this file.
  await until('VZ creates NEW.TXT in MEMFS', (text) =>
    !text.includes('Save As:') && vc.FS.analyzePath('/home/vc/new.txt').exists);
  assert.equal(vc.FS.readFile('/home/vc/new.txt', { encoding: 'utf8' }), 'New VZ.\r\n\x1a');
  send('\x1bq');
  await until('VZ quit confirmation after the new file is saved', (text) =>
    text.includes('Quit from editor? (Y/N)'));
  send('Y');
  await until('VC panels after the second VZ child', isPanel);
  assertFetches();
  assert.deepEqual(expectedFetches, programModules, 'all four programs fetched exactly once');
  console.log('PASS 25: typed vz NEW.TXT creates and saves a real H: file');

  stage = '25b cached BASIC after loading every side module';
  send('gwbasic\r');
  await until('an early-loaded side runs after loading all later libraries', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  send('PRINT 6*7\r');
  await until('the cached BASIC still uses the shared CPU and memory', (text) => /^\s*42\s*$/m.test(text));
  send('SYSTEM\r');
  await until('VC panels after the final cached BASIC', isPanel);
  assertFetches();
  // emmalloc never trims its arenas here: sbrk(0) is the allocator high-water,
  // including statics/stack, retained bytes and the guard's temporary probe.
  const peak = wasmExports.sbrk(0);
  console.log(`PASS memory: peak ${peak} bytes; INITIAL_MEMORY ${startupHeapBytes} bytes; headroom ${startupHeapBytes - peak} bytes; heap ${startupHeapBytes} -> ${vc.HEAPU8.byteLength}`);
  assert.equal(vc.HEAPU8.byteLength, startupHeapBytes,
    'INITIAL_MEMORY must cover loading all four programs without heap growth');
  // The measured peak depends on load order: emmalloc asks sbrk for a whole
  // new block when no free block fits. Bound every order: each load's guard
  // may take a fresh 2N + 64 KiB above the startup heap top.
  const sideBytes = [...programFiles.values()].reduce((sum, name) =>
    sum + statSync(join(dirname(modulePath), name)).size, 0);
  const worst = startupHeapTop + 2 * sideBytes + programFiles.size * 64 * 1024;
  assert.ok(startupHeapBytes - worst >= Math.max(4 * 1024 * 1024, worst * 0.2),
    `INITIAL_MEMORY leaves at least 4 MiB and 20% headroom above the any-order bound ${worst}`);
  console.log(`PASS memory: any-order bound ${worst} bytes; headroom ${startupHeapBytes - worst} bytes`);

  assert.equal(new Set(sourceFetches.map(({ url }) => url)).size, sourceFetches.length,
    'reopening Source reuses every previously fetched map and original source file');
  console.log(`PASS Source cache: ${sourceFetches.length} immutable lazy assets, ${sourceFetches.reduce((sum, item) => sum + item.bytes, 0)} fetched bytes`);
  sourcePanel.dispose();

  stage = '26 quit and exit hook';
  await quitVC();
  console.log('PASS 26: F10, Enter quits and fires the exit hook');
  assertFetches();
  console.log('web smoke: all 26 checks, Russian F3, Source for all five programs, and first-use/cached module fetches passed');
  exitAfterOutput(0);
} catch (error) {
  fail(error);
}
