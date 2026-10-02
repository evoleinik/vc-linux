// The real translated VC in Emscripten's MEMFS, with the same byte hooks as
// the page. Actions wait for screen state; timers only put a bound on failure.
// Add --fetch-failure, --fetch-timeout or --memory-limit for recovery gates.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { isMainThread, parentPort, Worker } from 'node:worker_threads';
import { fakeBBS, fixtureReplies } from './fake_bbs.mjs';

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
const kermitOnly = process.argv.includes('--kermit-only');
const msdosOnly = process.argv.includes('--msdos-only');
const vc405Only = process.argv.includes('--vc405-only');
const lazyFilesOnly = process.argv.includes('--lazy-files-only');
const fileFailure = process.argv.includes('--file-fetch-failure');
const hackOnly = process.argv.includes('--hack-only');
const sourceOnly = process.argv.includes('--source-only');
const lazyParentMove = process.argv.includes('--lazy-parent-move');
const lazyRmdir = process.argv.includes('--lazy-rmdir');
if (process.argv.includes('--no-webcrypto')) {
  Object.defineProperty(globalThis, 'crypto', { value: undefined, configurable: true });
  assert.equal(globalThis.crypto?.subtle, undefined, 'exercise ordinary HTTP without WebCrypto');
}
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
const modemSource = readFileSync(resolve(dirname(modulePath), 'modem.js'), 'utf8');
const { createModemTransport } = await import(`data:text/javascript,${encodeURIComponent(modemSource)}`);
const bbs = fakeBBS();
const modem = createModemTransport({ WebSocketClass: bbs.WebSocketClass });
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
const legacyModules = ['gwbasic.wasm', 'bootlogo.wasm', 'rogue.wasm', 'vz.wasm', 'kermit.wasm'];
const msdosFiles = ['COMMAND.COM', 'EDLIN.COM', 'DEBUG.COM', 'FIND.EXE', 'MORE.COM', 'SORT.EXE', 'FC.EXE'];
const msdosModules = msdosFiles.map(name => `${name.split('.')[0].toLowerCase()}.wasm`);
const vc405Modules = ['vc405.wasm', 'vcsetup405.wasm'];
const programModules = [...legacyModules, ...msdosModules, ...vc405Modules, 'hack.wasm'];
const programFiles = new Map(readdirSync(dirname(modulePath)).flatMap((name) => {
  const match = name.match(/^([a-z0-9]+)\.([0-9a-f]{12})\.wasm$/);
  return match && programModules.includes(`${match[1]}.wasm`) ? [[`${match[1]}.wasm`, name]] : [];
}));
assert.equal(programFiles.size, programModules.length, 'all side modules have immutable build-hash filenames');
const moduleFetches = [];
const expectedFetches = [];
const fileFetches = [];
// Mutations use isolated publication directories but the same frozen build
// inputs. Do not infer a second demo tree next to a private test site.
const demoDirectory = resolve(process.env.WEB_DEMO_DIRECTORY || fileURLToPath(new URL('../build/web-work/demo', import.meta.url)));
const demoBytes = name => readFileSync(join(demoDirectory, name));
const lazyAssetName = bytes => `file.${createHash('sha256').update(bytes).digest('hex').slice(0, 12)}.bin`;
let heldFile = null;
let holdFileName = null;
let heldDownload = null;
const speakerEvents = [];
const faultMessage = 'No translated code at 0000:0000. GWBASIC.EXE stopped.';
let sawFaultMessage = false;
let sawBBSBanner = false;
let sawModemConnect = false;
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
      if (screen.includes('ENiGMA') && screen.includes('BBS version')) sawBBSBanner = true;
      if (screen.includes('CONNECT 14400')) sawModemConnect = true;
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

// Hack keeps drawing for a moment after its first screen appears, so a
// before/after comparison must wait for a quiet screen (flaked 2026-10-02).
async function settledScreen(quietMs = 300, limitMs = 5000) {
  const start = Date.now();
  let last = screen, since = Date.now();
  while (Date.now() - start < limitMs) {
    await new Promise(done => realSetTimeout(done, 50));
    if (screen !== last) { last = screen; since = Date.now(); }
    else if (Date.now() - since >= quietMs) return screen;
  }
  return screen;
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
  assert.match(path, /^(?:asm|asm405|third_party)\//, 'source provenance names a vendored original');
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

async function fileRequest(url) {
  const resource = new URL(url);
  const name = basename(resource.pathname);
  const request = { name, url, bodyReads: 0 };
  fileFetches.push(request);
  assert.equal(resource.protocol, 'file:', 'lazy DOS files never use the network in Node');
  assert.equal(resource.search, `?v=${buildHash}`, 'lazy DOS files use the same stamped resolver');
  assert.match(name, /^file\.[0-9a-f]{12}\.bin$/, 'DOS contents have immutable filenames');
  if (holdFileName === name) {
    await new Promise(release => {
      heldFile = { name, release, screen, inputReads, outputCalls };
      observe();
    });
    heldFile = null;
  }
  await new Promise(done => setImmediate(done));
  return { request, resource };
}

async function fetchFileBytes(url) {
  const { request, resource } = await fileRequest(url);
  const bytes = await programBytes(resource);
  request.bodyReads++;
  assert.equal(lazyAssetName(bytes), request.name, 'the actual requested bytes match their immutable filename');
  return bytes;
}

async function fetchProgramBytes(url) {
  if (new URL(url).pathname.endsWith('.bin')) return fetchFileBytes(url);
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
  const file = programFiles.get(name);
  console.log(`PASS lazy: ${file}?v=${buildHash} first use waits quietly and yields to Node`);
}

async function releaseFileFetch(name, label) {
  await until(`${label} first-open file download request`, () => heldFile?.name === name);
  const held = heldFile;
  await new Promise(done => setImmediate(done));
  observe();
  assert.equal(screen, held.screen, 'a held file download leaves the DOS screen unchanged');
  assert.equal(inputReads, held.inputReads, 'DOS is suspended while the file downloads');
  assert.equal(outputCalls, held.outputCalls, 'the wait emits no guest output');
  assert.equal(exitCalls, 0, 'the file wait keeps VC alive');
  holdFileName = null;
  held.release();
  console.log(`PASS lazy file: ${label} first OPEN waits quietly and yields to Node`);
}

async function viewExactFile(name, path, bytes) {
  await selectFile(name);
  const asset = lazyAssetName(bytes);
  const firstFetch = !fileFetches.some(request => request.name === asset);
  if (firstFetch) holdFileName = asset;
  send('\x1bOR');
  if (firstFetch) await releaseFileFetch(asset, name);
  await until(`${name} appears in the real DOS viewer`, text =>
    !hasPanels(text) && text.toUpperCase().includes(name.toUpperCase()));
  assert.deepEqual(Buffer.from(vc.FS.readFile(path)), bytes, `${name}: DOS OPEN preserves every original byte`);
  send('\x1b');
  await until(`panels after viewing ${name}`, hasPanels);
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

const hackStatus = /Level\s+(\d+)\s+Gold\s+\d+\s+Hp\s+\d+\(\d+\)\s+Ac\s+-?\d+\s+Str\s+[^\n]+Exp\s+\d+/;
const isHack = text => hackStatus.test(text) && text.includes('@') && !hasPanels(text);

async function startHack() {
  await until('Hack asks its original experience question or restores a game', text =>
    /Are you an experienced player\?/.test(text) || isHack(text));
  if (!isHack(screen)) {
    send('y');
    await until('Hack asks for a character role', text => text.includes('what kind of character') && text.includes('? ['));
    send('C');
  }
  for (let prompts = 0; prompts < 8; prompts++) {
    await until('Hack first level, @ and Gold/Hp/Ac/Str/Exp status', text =>
      isHack(text) || text.includes('--More--'));
    if (!screen.includes('--More--')) break;
    const previous = screen;
    send(' ');
    await until('Hack acknowledges its displayed continuation prompt', text => text !== previous);
  }
  assert.ok(isHack(screen), 'Hack renders its original level and status');
  assert.equal(hackStatus.exec(screen)[1], '1', 'a fresh Hack starts on level one');
  assert.equal(graphics, null, 'Hack uses the BIOS text display');
}

async function quitHack() {
  send('Q');
  await until('Hack asks Really quit?', text => text.includes('Really quit?'));
  send('y');
  for (let prompts = 0; prompts < 8; prompts++) {
    await until('Hack returns to VC or asks to continue its score screen', text =>
      hasPanels(text) || text.includes('--More--') || text.includes('Hit <') || text.includes('Press ENTER to return'));
    if (hasPanels(screen)) break;
    const previous = screen;
    send(screen.includes('--More--') ? ' ' : '\r');
    await until('Hack acknowledges its displayed exit prompt', text => text !== previous);
  }
  assert.ok(hasPanels(screen), 'Hack Q/y returns to VC panels');
  assert.equal(exitCalls, 0, 'quitting Hack must not quit VC');
}

async function checkHack() {
  stage = 'Hack lazy files';
  const directory = '/home/vc/GAMES/HACK';
  const shipped = ['HACK.EXE', 'data', 'help', 'hh', 'rumors', 'record', 'perm',
    'OWLIC.TXT', 'HACKLIC.TXT', 'FENLIC.TXT'];
  assert.deepEqual(vc.FS.readdir(directory).sort(), ['.', '..', ...shipped].sort(),
    'every Hack executable, data file and licence is listed before the first OPEN');
  const before = fileFetches.length;
  for (const name of shipped) {
    const original = demoBytes('GAMES/HACK/' + name);
    assert.equal(vc.FS.stat(directory + '/' + name).size, original.length,
      name + ': unopened listing carries the exact file size');
    const asset = lazyAssetName(original);
    assert.deepEqual(readFileSync(join(dirname(modulePath), asset)), original,
      name + ': published first-open file is the complete reproducible asset');
    if (original.length) assert.throws(() => vc.FS.readFile(directory + '/' + name),
      name + ': a listing cannot expose or eagerly fetch lazy contents');
  }
  assert.equal(createHash('sha256').update(demoBytes('GAMES/HACK/HACK.EXE')).digest('hex'),
    'b6f6ca8667fc8d1e37eb81fbd1c469a371312e4a39c53052985d553aaac345ee');
  const existingPerm = 'existing session file survives lazy directory access';
  vc.FS.writeFile(directory + '/perm', existingPerm);
  send('cd GAMES\\HACK\r');
  await until('VC lists every unopened Hack file', text =>
    hasPanels(text) && commandLine(text).trimEnd() === 'H:\\GAMES\\HACK>');
  assert.equal(fileFetches.length, before, 'entering/listing HACK fetches no contents');
  assert.equal(vc.FS.readFile(directory + '/perm', { encoding: 'utf8' }), existingPerm,
    'directory access never overwrites a pre-existing session file');
  vc.FS.writeFile(directory + '/perm', demoBytes('GAMES/HACK/perm'));
  assert.equal(moduleFetches.length, 0, 'listing the data directory never fetches translated code');

  await viewExactFile('help', directory + '/help', demoBytes('GAMES/HACK/help'));
  assert.equal(fileFetches.length, before + 1, 'opening help fetches help alone, not a directory bundle');

  stage = 'Hack Enter launch';
  await selectFile('HACK.EXE');
  const executableAsset = lazyAssetName(demoBytes('GAMES/HACK/HACK.EXE'));
  holdFileName = executableAsset;
  expectedFetches.push('hack.wasm');
  send('\r');
  await releaseFileFetch(executableAsset, 'HACK.EXE');
  await releaseProgramFetch('hack.wasm');
  await startHack();
  assert.deepEqual(Buffer.from(vc.FS.readFile(directory + '/HACK.EXE')),
    demoBytes('GAMES/HACK/HACK.EXE'), 'EXEC reads the complete, byte-identical pinned executable');
  await quitHack();

  // Exercise the remaining read-only assets through real DOS opens. The
  // game's record/perm files are intentionally writable, never restored over
  // a user's score or lock data by a later first-open installation.
  for (const name of ['data', 'hh', 'rumors', 'OWLIC.TXT', 'HACKLIC.TXT', 'FENLIC.TXT'])
    await viewExactFile(name, directory + '/' + name, demoBytes('GAMES/HACK/' + name));
  const cachedFiles = fileFetches.length;
  assert.equal(fileFetches.filter(request => request.name === executableAsset).length, 1,
    'one successful executable fetch also supplies EXEC its private reference');
  send('cd ..\r');
  await until('GAMES panels after Hack', text => hasPanels(text) && commandLine(text).trimEnd() === 'H:\\GAMES>');
  send('cd ..\r');
  await until('H: root panels after Hack', text => isPanel(text) && commandLine(text).trimEnd() === 'H:\\>');

  stage = 'Hack cached typed launch';
  send('hack\r');
  await startHack();
  assertFetches();
  assert.equal(fileFetches.length, cachedFiles, 'typed Hack reuses its already opened file contents');
  if (sourcePanel) {
    stage = 'Source Hack function map';
    const beforeSource = await settledScreen();
    const sourceView = await openSource('HACK.EXE');
    assert.equal(typeof sourceView.current.function, 'string', 'Hack reports a compiled C function');
    assert.ok(sourceView.current.function.length > 0);
    assert.ok(Number.isInteger(sourceView.current.offset) && sourceView.current.offset >= 0);
    assert.equal(sourceView.current.label, 'C function (map)');
    assert.ok(sourceView.current.line == null && sourceView.current.path == null,
      'Hack must not invent a C source filename or line');
    const linkerMap = readFileSync(new URL('../build/hack/HACK.MAP', import.meta.url), 'utf8');
    const symbols = [...linkerMap.matchAll(/^([0-9a-f]{4}):([0-9a-f]{4})[*+]?\s+(\S+)\s*$/gim)];
    assert.equal(sourceView.current.address, sourceView.snapshot.current.offset);
    assert.ok(symbols.some(([, segment, offset, name]) => name === sourceView.current.function
      && parseInt(segment, 16) * 16 + parseInt(offset, 16) + sourceView.current.offset === sourceView.current.address),
    'the reported Hack function name and offset agree with its linker map');
    assert.ok(sourceElement.textContent.includes(sourceView.current.function));
    assert.match(sourceElement.textContent, /C function[^\n]*map/);
    await closeSourceWithShortcut();
    assert.equal(await settledScreen(), beforeSource, 'opening and closing Source preserves Hack\'s screen');
    console.log('PASS Source Hack: real C function name/offset and map label, unchanged game screen');
  }
  await quitHack();
  await until('VC root panels after cached Hack', text =>
    isPanel(text) && commandLine(text).trimEnd() === 'H:\\>');
  assert.ok(!vc.FS.analyzePath('/home/vc/HACK.EXE').exists,
    'HACK resolves through its own directory, without a root copy');
  console.log('PASS Hack: first-screen metadata, per-OPEN exact assets/licences, Enter launch, first-level @/status, Q/y, and cached typed launch');
}

async function checkLazySources() {
  stage = 'lazy H: source files';
  const before = fileFetches.length;
  assert.ok(vc.FS.lookupPath('/home/vc/SRC/VCOVL.ASM').node.vcLazyFile,
    'the unopened source still has its per-file lazy marker');
  await selectFile('SRC');
  send('\x1b[17~');
  await until('F6 offers to rename the unopened source directory', text => text.includes('Rename or move'));
  send('SOURCE\r');
  await until('renaming a lazy directory moves its original metadata too', text =>
    isPanel(text) && vc.FS.analyzePath('/home/vc/SOURCE/VCOVL.ASM').exists);
  assert.ok(!vc.FS.analyzePath('/home/vc/SRC').exists, 'F6 moved the directory, not just a copied preview');
  assert.ok(vc.FS.lookupPath('/home/vc/SOURCE/VCOVL.ASM').node.vcLazyFile,
    'renaming a parent retains the unopened inode marker');
  assert.equal(fileFetches.length, before, 'an unopened directory move never downloads file contents');
  await selectFile('SOURCE');
  send('\x1b[17~');
  await until('F6 can rename the source directory back', text => text.includes('Rename or move'));
  send('SRC\r');
  await until('the source directory is restored under its original name', text =>
    isPanel(text) && vc.FS.analyzePath('/home/vc/SRC/VCOVL.ASM').exists);
  assert.equal(fileFetches.length, before, 'moving the directory back is metadata-only too');
  send('cd SRC\r');
  await until('VC lists the still-lazy source files', text =>
    hasPanels(text) && commandLine(text).trimEnd() === 'H:\\SRC>');
  for (const name of ['VC.ASM', 'VCOVL.ASM']) {
    const original = readFileSync(new URL('../asm/' + name, import.meta.url));
    await viewExactFile(name, '/home/vc/SRC/' + name, original);
    assert.equal(fileFetches.filter(request => request.name === lazyAssetName(original)).length, 1,
      name + ': exactly one successful first-OPEN download');
  }
  send('cd ..\r');
  await until('H: root panels after browsing sources', text => isPanel(text) && commandLine(text).trimEnd() === 'H:\\>');
  console.log('PASS H: sources: F6 preserves unopened file metadata without downloads; later DOS opens fetch exact files once');
}

async function checkLazyDirectoryOperation() {
  const directory = '/home/vc/GAMES/HACK';
  assert.ok(vc.FS.lookupPath(directory + '/HACK.EXE').node.vcLazyFile,
    'directory operation begins with a listed but unopened executable');
  const hackAssets = new Set(['HACK.EXE', 'data', 'help', 'hh', 'rumors', 'record', 'perm',
    'OWLIC.TXT', 'HACKLIC.TXT', 'FENLIC.TXT'].map(name => lazyAssetName(demoBytes('GAMES/HACK/' + name))));
  assert.ok(!fileFetches.some(request => hackAssets.has(request.name)), 'no Hack file content was requested at startup');
  if (lazyRmdir) {
    stage = 'RMDIR of an unopened lazy directory';
    const loads = dosLoads('COMMAND.COM');
    expectedFetches.push('command.wasm');
    send('COMMAND.COM /C RMDIR GAMES\\HACK\r');
    await releaseProgramFetch('command.wasm');
    await dosPanels('COMMAND.COM', loads);
    await savedDosScreen('DOS refuses removal of the nonempty lazy directory', text => text.includes('directory not empty'));
    assert.ok(vc.FS.lookupPath(directory + '/HACK.EXE').node.vcLazyFile,
      'RMDIR preserves the still-unopened executable without downloading it');
    assert.ok(!fileFetches.some(request => hackAssets.has(request.name)),
      'RMDIR needs only the already-listed metadata, never Hack file bytes');
    console.log('PASS lazy RMDIR: real DOS nonempty-directory error preserves Hack without any Hack download');
  } else {
    stage = 'move a parent of an unopened lazy directory';
    await selectFile('GAMES');
    send('\x1b[17~');
    await until('F6 offers to rename GAMES', text => text.includes('Rename or move'));
    send('PLAY\r');
    await until('moving GAMES carries its still-unopened Hack subtree', text =>
      isPanel(text) && vc.FS.analyzePath('/home/vc/PLAY/HACK/HACK.EXE').exists);
    assert.ok(vc.FS.lookupPath('/home/vc/PLAY/HACK/HACK.EXE').node.vcLazyFile,
      'moving an unopened ancestor preserves the executable lazy marker');
    assert.equal(fileFetches.length, 0, 'moving an unopened ancestor performs no file downloads');
    send('cd PLAY\\HACK\r');
    await until('panels in the moved Hack directory', text =>
      hasPanels(text) && commandLine(text).trimEnd() === 'H:\\PLAY\\HACK>');
    assert.equal(fileFetches.length, 0, 'listing the moved directory remains metadata-only');
    await viewExactFile('HACK.EXE', '/home/vc/PLAY/HACK/HACK.EXE', demoBytes('GAMES/HACK/HACK.EXE'));
    assert.equal(fileFetches.length, 1, 'opening a moved file fetches only that file');
    assert.equal(moduleFetches.length, 0, 'moving/viewing game files never loads their translated code');
    console.log('PASS lazy parent move: F6 carries unopened Hack metadata; first OPEN at its new path fetches exact bytes');
  }
  assertFetches();
}

async function selectFile(name) {
  // The status row, not the listing (where every name is always visible),
  // identifies the active selection. Home makes this independent of which
  // file a preceding viewer or nested program left selected; Down does not
  // wrap in either original VC. Wait for the key to land before advancing.
  if (selected(screen).toUpperCase().includes(name.toUpperCase())) return;
  const reads = inputReads;
  send('\x1b[H');
  await until('Home reaches the active panel', () => pending.length === 0 && inputReads > reads + 1);
  for (let step = 0; step < 40; step++) {
    const previous = selected(screen);
    if (previous.toUpperCase().includes(name.toUpperCase())) return;
    send('\x1b[B');
    await until('the selection to move', (text) => selected(text) !== previous);
  }
  assert.fail(`${name} was not reachable in the active panel`);
}

async function checkLazyFiles() {
  stage = 'lazy H: metadata, first DOS open, retry and cache';
  const original = demoBytes('SRC/VC.ASM');
  const firstLine = original.toString('ascii').split(/\r?\n/)[0];
  const name = lazyAssetName(original);
  const path = '/home/vc/SRC/VC.ASM';
  const before = vc.FS.stat(path);
  assert.equal(fileFetches.filter(item => item.name === name).length, 0,
    'merely listing H: and its complete SRC directory never fetches VC.ASM');
  await selectFile('SRC');
  send('\r');
  await until('SRC panel before lazy F3', text => hasPanels(text) && commandLine(text).includes('SRC>'));
  await selectFile('VC.ASM');
  const logStart = dosLog().length;
  holdFileName = name;
  send('\x1bOR');
  await until('F3 reaches the lazy DOS-open fetch', () => heldFile?.name === name);
  const held = heldFile;
  await new Promise(done => setImmediate(done));
  observe();
  assert.equal(screen, held.screen, 'a held file download cannot change the DOS screen');
  assert.equal(inputReads, held.inputReads, 'DOS is suspended while the file downloads');
  assert.equal(outputCalls, held.outputCalls, 'the wait emits no guest output');
  assert.equal(exitCalls, 0, 'the file wait keeps VC alive');
  held.release();
  holdFileName = null;
  if (fileFailure) {
    // VC's viewer uses the same generic dialog for every failed open.
    // AH=716Ch names its file through DS:SI, while the existing trace
    // prints DS:DX; the immediately following AH=59h supplies the name.
    await until('failed file fetch reaches the original viewer error dialog', text =>
      text.includes("Can't find the file") && text.includes('VC.ASM'));
    assert.match(dosLog().slice(logStart),
      /int21 716C[^\n]*CF=1 AX=0005\nint21 5905[^\n]*"VC\.ASM"[^\n]*CF=0 AX=0005/,
      'the real DOS open, not merely the UI, reports error 5');
    assert.equal(fileFetches.find(item => item.name === name).bodyReads, 0,
      'HTTP-error file bodies must not be read or installed');
    assert.equal(vc.FS.stat(path).size, original.length, 'a failed fetch preserves listing metadata');
    assert.throws(() => vc.FS.readFile(path), 'failed placeholders cannot expose fabricated zero content');
    send('\x1b');
    await until('VC remains usable after failed file fetch', hasPanels);
    send('\x1bOR');
  }
  await until('F3 shows the first original H: SRC VC.ASM line', text => text.includes(firstLine));
  assert.deepEqual(Buffer.from(vc.FS.readFile(path)), original, 'DOS open materializes every original byte');
  const after = vc.FS.stat(path);
  assert.equal(after.ino, before.ino, 'materialization preserves the listed inode');
  assert.equal(after.size, before.size, 'materialization preserves the listed size');
  assert.equal(after.mtime.getTime(), before.mtime.getTime(), 'materialization preserves the listed date');
  send('\x1b');
  await until('SRC panel after first viewer', hasPanels);
  send('\x1bOR');
  await until('second F3 reads cached source contents', text => text.includes(firstLine));
  assert.equal(fileFetches.filter(item => item.name === name).length, fileFailure ? 2 : 1,
    'exactly one successful fetch, with only the intentional failed attempt retried');
  assert.equal(exitCalls, 0, 'both F3 opens leave VC running');
  send('\x1b');
  await until('SRC panel after cached viewer', hasPanels);
  send('\x1b[H');
  await until('parent selected after cached viewer', text => selected(text).includes('..'));
  send('\r');
  await until('root panels after lazy-file test', isPanel);
  console.log(`PASS lazy H: complete metadata, suspended DOS open, F3 ${JSON.stringify(firstLine)}, one successful fetch${fileFailure ? ', HTTP503 DOS error5 and working retry' : ''}`);
}

async function checkVC405() {
  stage = 'VC 4.05 Enter, Source, F3, private setup and F10';
  const modernPath = '/var/vc/config/vc-linux/VC.INI';
  const modern = vc.FS.readFile(modernPath).slice();
  const privateIni = '/home/vc/VC405/VC.INI';
  const oldPanels = text => (text.split('\n')[0] || '').includes('╔')
    && (text.split('\n')[24] || '').includes('10Quit');
  await selectFile('VC405');
  send('\r');
  await until('the VC405 directory in the newer VC', text => hasPanels(text) && commandLine(text).includes('VC405>'));
  await selectFile('VC.COM');
  const firstLoad = dosLoads('VC405.COM');
  expectedFetches.push('vc405.wasm');
  send('\r');
  await releaseProgramFetch('vc405.wasm');
  await until('Enter starts actual VC 4.05 with its source-default right panel', text =>
    dosLoads('VC405.COM') > firstLoad && oldPanels(text) && text.includes('Version 4.05'));
  assert.ok(!hasPanels(screen), 'unedited 4.05 defaults initially hide the inactive left panel');
  assert.match(screen.toLowerCase(), /license/, 'the older panel lists actual H: files');
  assert.deepEqual(Buffer.from(vc.FS.readFile('/home/vc/VC405/VC.COM')),
    demoBytes('VC405/VC.COM'), '4.05 runs the complete TASM-identical file');
  let view = await openSource('VC405.COM');
  assertSourceLine(view.current);
  assert.match(view.current.path, /^asm405\//, 'running 4.05 maps to its own unedited source');
  for (const caller of view.callers) {
    assertSourceLine(caller);
    assert.match(caller.path, /^asm405\//, '4.05 callers never borrow newer VC source lines');
  }
  await closeSourceWithShortcut();
  await selectFile('LICENSE.TXT');
  const firstLine = demoBytes('VC405/LICENSE.TXT').toString('ascii').split(/\r?\n/)[0];
  send('\x1bOR');
  await until('4.05 F3 displays its licence first line', text => text.includes(firstLine));
  send('\x1b');
  await until('4.05 panels after F3', oldPanels);
  send('\x1b[20;2~');
  await until('4.05 asks to save its own setup', text => text.includes('Do you wish to save'));
  // The page sends physical key-up reports; the raw-byte harness must too.
  send('\x1b[57441;1:3u\r');
  await until('4.05 writes its private VC.INI', text => oldPanels(text) && vc.FS.analyzePath(privateIni).exists);
  const saved = Buffer.from(vc.FS.readFile(privateIni));
  assert.equal(saved.subarray(0, 3).toString('ascii'), 'VVV', '4.05 wrote an actual settings file');
  assert.notDeepEqual(saved, Buffer.from(modern), '4.05 has its own incompatible settings format');
  assert.deepEqual(vc.FS.readFile(modernPath), modern, '4.05 saving cannot overwrite 4.99 settings');

  const returned = dosLoads('VC.OVL');
  send('\x1b[21~');
  await until('4.05 F10 quit confirmation', text => text.includes('Do you want to quit'));
  send('\r');
  await until('4.05 F10 returns to the newer panels', text =>
    (dosLoads('VC.OVL') > returned && hasPanels(text)) || text.includes('Press ENTER'));
  if (screen.includes('Press ENTER')) send('\r');
  await until('the newer VC restores both panels', text => dosLoads('VC.OVL') > returned && hasPanels(text));
  assert.equal(exitCalls, 0, '4.05 F10 exits only the nested program');
  assert.deepEqual(vc.FS.readFile(modernPath), modern, '4.05 automatic save also remains isolated');

  await selectFile('VCSETUP.COM');
  const setupLoad = dosLoads('VCSETUP.COM');
  expectedFetches.push('vcsetup405.wasm');
  send('\r');
  await releaseProgramFetch('vcsetup405.wasm');
  await until('the original 4.05 setup menu', text =>
    dosLoads('VCSETUP.COM') > setupLoad && text.includes('F2   Configuration'));
  assert.deepEqual(Buffer.from(vc.FS.readFile('/home/vc/VC405/VCSETUP.COM')),
    demoBytes('VC405/VCSETUP.COM'), 'setup also executes complete TASM-identical bytes');
  view = await openSource('VCSETUP.COM');
  assertSourceLine(view.current);
  assert.match(view.current.path, /^asm405\//, 'setup maps to asm405 too');
  await closeSourceWithShortcut();
  send('\x1bOQ');
  await until('4.05 setup opens Configuration', text => text.includes('Auto menus'));
  const option = screen.split('\n').find(row => row.includes('Auto menus'));
  send(' ');
  await until('setup changes an actual configuration option', text =>
    text.split('\n').find(row => row.includes('Auto menus')) !== option);
  const quickExecute = screen.split('\n').find(row => row.includes('Quick execute commands'));
  send('\x1b[C\x1b[B ');
  await until('setup enables the real 4.05 INT 2e command path', text =>
    text.split('\n').find(row => row.includes('Quick execute commands')) !== quickExecute);
  send('\r');
  await until('the setup main menu after editing', text => text.includes('F2   Configuration'));
  const setupReturn = dosLoads('VC.OVL');
  send('\x1b[21~');
  await until('4.05 setup asks to save the changed settings', text => text.includes('Do you wish to save'));
  send('\r');
  await until('setup returns to newer VC', text =>
    (dosLoads('VC.OVL') > setupReturn && hasPanels(text)) || text.includes('Press ENTER'));
  if (screen.includes('Press ENTER')) send('\r');
  await until('newer panels after setup', text => dosLoads('VC.OVL') > setupReturn && hasPanels(text));
  const updated = Buffer.from(vc.FS.readFile(privateIni));
  assert.notDeepEqual(updated, saved, 'the real setup modifies the private 4.05 file');
  assert.equal(updated.subarray(0, -2).reduce((sum, byte) => sum + byte, 0) & 0xffff,
    updated.readUInt16LE(updated.length - 2), '4.05 setup writes its original settings checksum');
  assert.deepEqual(vc.FS.readFile(modernPath), modern, 'setup leaves the newer VC.INI unchanged');
  assert.equal(exitCalls, 0);
  await checkNestedVC405(oldPanels, privateIni, updated, modernPath);
  send('\x1b[H');
  await until('parent selected after setup', text => selected(text).includes('..'));
  send('\r');
  await until('H: root after both 4.05 programs', isPanel);
  assertFetches();
  console.log('PASS VC405: Enter, original 4.05 panels, F3, asm405 Source for both images, private VC.INI, F10 returns; both modules fetched once');
}

async function checkNestedVC405(oldPanels, privateIni, expectedIni, modernPath) {
  stage = 'VC 4.05 -> COMMAND -> H:\\.VC\\VC.COM: nested save and clean 4.05 restart';
  // Web keeps its installed 4.99 files off H:. Reproduce the door's .VC
  // directory with complete installed bytes so the exact reported path
  // traverses COMMAND's real DOS loader, not a harness launch shortcut.
  const nestedDirectory = '/home/vc/.VC';
  const nestedIni = `${nestedDirectory}/VC.INI`;
  vc.FS.mkdirTree(nestedDirectory);
  for (const name of ['VC.COM', 'VC.OVL', 'VC.INI', 'VC.EXT', 'VCEDIT.EXT'])
    vc.FS.writeFile(`${nestedDirectory}/${name}`, vc.FS.readFile(`${dirname(modernPath)}/${name}`));
  const modernBefore = Buffer.from(vc.FS.readFile(modernPath));
  const oldReady = text => oldPanels(text) && !hasPanels(text);
  const badIni = text => text.includes('VC.INI is not correct');
  const autoMenu = text => text.includes('Could not find the menu file') && text.includes('H:\\VC405\\vc.mnu');
  async function startOld() {
    await selectFile('VC.COM');
    const before = dosLoads('VC405.COM');
    send('\r');
    // VCSETUP above enabled Auto menus. No VC.MNU is installed, so its
    // automatic menu dialog proves the restarted guest read that setting.
    await until('4.05 restarts with its saved Auto menus option', text =>
      dosLoads('VC405.COM') > before && (autoMenu(text) || badIni(text)));
    assert.ok(!badIni(screen), '4.05 must accept its own VC.INI without a corruption warning');
    send('\r');
    await until('4.05 panels after its automatic user-menu dialog', text => oldReady(text) && !autoMenu(text));
  }
  async function quitOld() {
    const before = dosLoads('VC.OVL');
    send('\x1b[21~');
    await until('4.05 nested-save test quit confirmation', text => text.includes('Do you want to quit'));
    send('\r');
    await until('4.05 exits back to the outer 4.99', text =>
      (dosLoads('VC.OVL') > before && hasPanels(text)) || text.includes('Press ENTER'));
    if (screen.includes('Press ENTER')) send('\r');
    await until('outer 4.99 panels after the nested-save test', text =>
      dosLoads('VC.OVL') > before && hasPanels(text));
  }
  await startOld();
  stage = 'VC 4.05 -> H:\\.VC\\VC.COM: ordinary EXEC nested save';
  const directLoads = dosLoads('VC.COM');
  const directLog = dosLog().length;
  const directModified = vc.FS.stat(nestedIni).mtime.getTime();
  send('H:\\.VC\\VC.COM\r');
  await until('4.05 quick execute starts the actual nested 4.99 panels', text =>
    dosLoads('VC.COM') > directLoads && oldPanels(text));
  assert.match(dosLog().slice(directLog), /load H:\\\.VC\\VC\.COM: translation VC\.COM\n/,
    'the real 4.05 quick-execute setting reaches ordinary DOS EXEC');
  send('\x1b[20;2~');
  await until('ordinary EXEC nested 4.99 Shift-F9 save dialog', text => text.includes('Save the current setup as'));
  const directSaveDialog = screen;
  const directReads = inputReads;
  send('\x1b[57441;1:3u\r');
  await until('ordinary EXEC nested 4.99 completes its settings save', text =>
    oldPanels(text) && !text.includes('Save the current setup as') && pending.length === 0 && inputReads > directReads + 1);
  assert.deepEqual(Buffer.from(vc.FS.readFile(privateIni)), expectedIni,
    'ordinary EXEC nested 4.99 Shift-F9 must not overwrite the private 4.05 VC.INI');
  assert.ok(directSaveDialog.includes('H:\\.VC\\VC.INI'), 'ordinary EXEC 4.99 saves in its own directory');
  assert.ok(vc.FS.stat(nestedIni).mtime.getTime() > directModified, 'ordinary EXEC 4.99 really writes its own INI');
  assert.deepEqual(Buffer.from(vc.FS.readFile(modernPath)), modernBefore,
    'ordinary EXEC nested 4.99 leaves the outer 4.99 settings unchanged');
  send('\x1b[21~');
  await until('ordinary EXEC nested 4.99 quit confirmation', text => text.includes('Do you want to quit'));
  send('\r');
  await until('ordinary EXEC nested 4.99 returns to 4.05', oldReady);
  console.log('PASS nested VC ordinary EXEC: 4.05 quick execute -> H:\\.VC\\VC.COM, real 4.99 Shift-F9 preserves the private 4.05 INI');

  stage = 'VC 4.05 -> COMMAND -> H:\\.VC\\VC.COM: nested save and clean 4.05 restart';
  const shellLoads = dosLoads('COMMAND.COM');
  const firstShell = !expectedFetches.includes('command.wasm');
  if (firstShell) expectedFetches.push('command.wasm');
  send('command\r');
  if (firstShell) await releaseProgramFetch('command.wasm');
  const lastLine = text => text.split('\n').filter(line => line.trim()).at(-1)?.trimEnd() || '';
  const shellPrompt = text => !oldPanels(text) && /^H:\\[^>\n]*>$/.test(lastLine(text));
  await until('4.05 opens the actual COMMAND prompt', text =>
    dosLoads('COMMAND.COM') > shellLoads && shellPrompt(text));
  send('set\r');
  await until('COMMAND inherits the private 4.05 VC variable', text =>
    /^VC=H:\\VC405\s*$/m.test(text) && shellPrompt(text));
  // DOS 2 parses the executable token as an FCB, so an absolute token
  // beginning H:\ is only a drive switch. CD then VC executes that same
  // exact H:\.VC\VC.COM file through its original supported command path.
  send('cd H:\\.VC\r');
  await until('COMMAND changes to the exact nested 4.99 directory', text =>
    shellPrompt(text) && lastLine(text) !== 'H:\\VC405>');
  const nestedDosDirectory = lastLine(screen).slice(0, -1);
  const nestedLoads = dosLoads('VC.COM');
  const modernModified = vc.FS.stat(nestedIni).mtime.getTime();
  send('vc\r');
  await until('COMMAND starts the nested 4.99 panels', text =>
    dosLoads('VC.COM') > nestedLoads && (oldPanels(text) ||
      (shellPrompt(text) && text.includes('Error reading overlay file.'))));
  assert.match(dosLog(), /load [^\n]*\/\.VC\/VC\.COM: translation VC\.COM \(DOS-hosted loader\)/,
    'the exact H:\\.VC\\VC.COM chain uses Microsoft COMMAND\'s own EXEC');
  assert.ok(oldPanels(screen), 'nested 4.99 must reach its panels, not fail on its DOS2 environment trailer');
  send('\x1b[20;2~');
  await until('nested 4.99 Shift-F9 save dialog', text => text.includes('Save the current setup as'));
  const saveDialog = screen;
  const reads = inputReads;
  send('\x1b[57441;1:3u\r');
  await until('nested 4.99 completes its real settings save', text =>
    oldPanels(text) && !text.includes('Save the current setup as') && pending.length === 0 && inputReads > reads + 1);
  assert.deepEqual(Buffer.from(vc.FS.readFile(privateIni)), expectedIni,
    'nested 4.99 Shift-F9 must not overwrite the private 4.05 VC.INI');
  assert.ok(saveDialog.includes(`${nestedDosDirectory}\\VC.INI`), '4.99 saves beside its own executable, not under VC405');
  assert.ok(vc.FS.stat(nestedIni).mtime.getTime() > modernModified, '4.99 really writes its own settings file');
  const modernSaved = Buffer.from(vc.FS.readFile(nestedIni));
  assert.equal(modernSaved.length, modernBefore.length, 'the nested save retains the 4.99 settings format');
  assert.equal(modernSaved.subarray(0, -2).reduce((sum, byte) => sum + byte, 0) & 0xffff,
    modernSaved.readUInt16LE(modernSaved.length - 2), 'the actual 4.99 save has its original checksum');
  assert.deepEqual(Buffer.from(vc.FS.readFile(modernPath)), modernBefore,
    'the nested 4.99 save also leaves the outer 4.99 settings alone');
  send('\x1b[21~');
  await until('nested 4.99 quit confirmation', text => text.includes('Do you want to quit'));
  send('\r');
  await until('nested 4.99 returns to COMMAND', shellPrompt);
  send('cd H:\\VC405\r');
  await until('COMMAND restores the original 4.05 directory', text =>
    shellPrompt(text) && lastLine(text) === 'H:\\VC405>');
  send('exit\r');
  await until('COMMAND returns to 4.05', oldReady);
  await quitOld();
  // 4.05's own Auto save can legitimately update its panel state on exit.
  // Restart must accept that private-format file and retain Auto menus.
  const beforeRestart = Buffer.from(vc.FS.readFile(privateIni));
  assert.equal(beforeRestart.length, expectedIni.length, '4.05 keeps its own settings format on exit');
  await startOld();
  assert.deepEqual(Buffer.from(vc.FS.readFile(privateIni)), beforeRestart,
    '4.05 cleanly reads its own persisted settings after both nested 4.99 saves');
  await quitOld();
  assert.equal(exitCalls, 0, 'the entire nested chain leaves the outer VC running');
  assertFetches();
  console.log('PASS nested VC: 4.05 -> COMMAND -> H:\\.VC\\VC.COM, 4.99 Shift-F9 writes its own INI, private 4.05 settings unchanged, clean 4.05 restart retains Auto menus');
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

function dosLog() {
  return vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' });
}

function dosLoads(program) {
  return dosLog().split('\n').filter(line =>
    line.endsWith(`translation ${program}`) ||
    line.endsWith(`translation ${program} (DOS-hosted loader)`)).length;
}

async function dosPanels(program, previousLoads) {
  await until(`${program} finishes and VC redraws both panels`, text =>
    dosLoads(program) > previousLoads && isPanel(text) && pending.length === 0 &&
    commandLine(text).trimEnd() === 'H:\\>');
  assert.equal(exitCalls, 0, `${program} must exit only its DOS child`);
  const log = dosLog();
  assert.doesNotMatch(log.slice(log.lastIndexOf(`translation ${program}`)),
    /No translated code|Incorrect DOS version/,
    `${program} uses its source-built translation and DOS-version compatibility`);
}

async function savedDosScreen(description, check) {
  send('\x0f');
  await until(description, text => !hasPanels(text) && check(text));
  send('\x0f');
  await until('VC panels after inspecting DOS output', isPanel);
}

async function checkCommand() {
  stage = 'DOS installation';
  for (const name of msdosFiles) {
    const path = name === 'COMMAND.COM' ? `/home/vc/${name}` : `/home/vc/DOS/${name}`;
    assert.equal(vc.FS.stat(path).size,
      readFileSync(new URL(`../build/msdos2/${name}`, import.meta.url)).length,
      `${path} lists the complete source-built DOS executable size before open`);
    if (name !== 'COMMAND.COM') assert.ok(!vc.FS.analyzePath(`/home/vc/${name}`).exists,
      'the six utilities belong in H:\\DOS, not H:\\');
  }
  assert.deepEqual(demoBytes('DOS/DOS.TXT'),
    readFileSync(new URL('../data/DOS.TXT', import.meta.url)), 'H:\\DOS has the DOS guide');
  assert.deepEqual(demoBytes('DOS/DOSLIC.TXT'),
    readFileSync(new URL('../third_party/msdos2/LICENSE', import.meta.url)), 'the MIT licence travels with DOS');

  stage = 'DOS DIR from VC command line';
  const beforeDir = dosLoads('COMMAND.COM');
  expectedFetches.push('command.wasm');
  send('dir\r');
  await releaseProgramFetch('command.wasm');
  await dosPanels('COMMAND.COM', beforeDir);
  // A complete root listing is taller than 25 rows. Its tail still shows
  // these actual H: files; Ctrl-O exposes VC's saved DOS output screen.
  await savedDosScreen('Microsoft DIR lists real H: files', text =>
    /README\s+TXT/.test(text) && /VZ\s+COM/.test(text) && /File\(s\)/.test(text));
  assertFetches();
  console.log('PASS DOS DIR: plain dir lazy-loads COMMAND.COM and lists H: files on VC\'s saved user screen');

  stage = 'DOS two-line batch from VC command line';
  vc.FS.writeFile('/home/vc/B30.BAT', 'echo batch-first-30\r\necho batch-second-30\r\n');
  const beforeBatch = dosLoads('COMMAND.COM');
  send('B30\r');
  await dosPanels('COMMAND.COM', beforeBatch);
  await savedDosScreen('both lines of a .BAT file execute in COMMAND.COM', text =>
    /^batch-first-30\s*$/m.test(text) && /^batch-second-30\s*$/m.test(text));
  assertFetches();
  console.log('PASS DOS batch: two lines run through cached COMMAND.COM and return to VC');

  stage = 'DOS interactive prompt and COMSPEC';
  const beforePrompt = dosLoads('COMMAND.COM');
  send('command\r');
  await until('COMMAND opens its own H: prompt', text =>
    dosLoads('COMMAND.COM') > beforePrompt && !hasPanels(text) && /^H:\\>\s*$/m.test(text));
  send('set\r');
  await until('the DOS environment has the real COMMAND and utility PATH', text =>
    text.includes('COMSPEC=H:\\COMMAND.COM') && /PATH=[^\n]*H:\\DOS(?:;|\s|$)/.test(text));
  send('exit\r');
  await dosPanels('COMMAND.COM', beforePrompt);
  assertFetches();
  console.log('PASS DOS prompt: COMMAND, COMSPEC=H:\\COMMAND.COM, H:\\DOS on PATH, EXIT restores VC');
}

async function checkDosUtilities() {
  stage = 'DOS EDLIN first use';
  const beforeEdlin = dosLoads('EDLIN.COM');
  expectedFetches.push('edlin.wasm');
  send('edlin E30.TXT\r');
  await releaseProgramFetch('edlin.wasm');
  await until('EDLIN new file and its * command prompt', text =>
    text.includes('New file') && /^\*\s*$/m.test(text));
  send('i\r');
  await until('EDLIN numbered insertion prompt', text => /^\s*1:\*?\s*$/m.test(text));
  send('edlin-web-30\r');
  await until('EDLIN acknowledges the inserted line', text =>
    text.includes('edlin-web-30') && /^\s*2:\*?\s*$/m.test(text));
  send('\x1a\r');
  await until('Ctrl-Z ends EDLIN insertion', text => /^\*\s*$/m.test(text));
  send('e\r');
  await dosPanels('EDLIN.COM', beforeEdlin);
  assert.equal(vc.FS.readFile('/home/vc/E30.TXT', { encoding: 'utf8' }), 'edlin-web-30\r\n\x1a');
  const beforeEdlinAgain = dosLoads('EDLIN.COM');
  send('edlin E30.TXT\r');
  await until('cached EDLIN opens the saved file', text =>
    dosLoads('EDLIN.COM') > beforeEdlinAgain && text.includes('End of input file') && /^\*\s*$/m.test(text));
  send('q\r');
  await until('EDLIN asks before abandoning the buffer', text => text.includes('Abort edit (Y/N)?'));
  send('y');
  await dosPanels('EDLIN.COM', beforeEdlinAgain);
  assertFetches();
  console.log('PASS DOS EDLIN: source editor saves exact MEMFS bytes and its second run is cached');

  for (const first of [true, false]) {
    stage = `DOS DEBUG ${first ? 'first' : 'cached'} use`;
    const before = dosLoads('DEBUG.COM');
    if (first) expectedFetches.push('debug.wasm');
    send('debug\r');
    if (first) await releaseProgramFetch('debug.wasm');
    await until('DEBUG opens its - prompt', text =>
      dosLoads('DEBUG.COM') > before && !hasPanels(text) && /^-\s*$/m.test(text));
    send('q\r');
    await dosPanels('DEBUG.COM', before);
    assertFetches();
  }
  console.log('PASS DOS DEBUG: - prompt and Q return on first and cached runs');

  const input = 'beta\r\nalpha\r\nx-line\r\n';
  vc.FS.writeFile('/home/vc/D30IN.TXT', input);
  vc.FS.writeFile('/home/vc/D30ALT.TXT', 'zeta\r\nalpha\r\nx-line\r\n');
  for (const [program, command, output, check] of [
    ['FIND.EXE', 'find "x" D30IN.TXT > F30.TXT', 'F30.TXT', text =>
      text.includes('x-line\r\n') && !text.includes('beta') && !text.includes('alpha')],
    // MORE.ASM initializes its cursor by printing CRLF before reading stdin.
    ['MORE.COM', 'more < D30IN.TXT > M30.TXT', 'M30.TXT', text => text === `\r\n${input}`],
    ['FC.EXE', 'fc /b D30IN.TXT D30ALT.TXT > C30.TXT', 'C30.TXT', text =>
      text.includes('--ADDRS----F1---F2-') && /00000000\s+62\s+7A/.test(text)],
    ['SORT.EXE', 'sort < D30IN.TXT > S30.TXT', 'S30.TXT', text => text === 'alpha\r\nbeta\r\nx-line\r\n'],
  ]) {
    const module = `${program.split('.')[0].toLowerCase()}.wasm`;
    for (const first of [true, false]) {
      stage = `DOS ${program} ${first ? 'first' : 'cached'} use`;
      const before = dosLoads(program);
      if (first) expectedFetches.push(module);
      send(`${command}\r`);
      if (first) await releaseProgramFetch(module);
      await dosPanels(program, before);
      const text = vc.FS.readFile(`/home/vc/${output}`, { encoding: 'utf8' });
      if (!check(text)) {
        send('\x0f');
        await until(`${program} saved error screen`, screen => !hasPanels(screen));
        console.error(`--- ${program} saved DOS output ---\n${screen}`);
      }
      assert.ok(check(text), `${program} produces its expected DOS output: ${JSON.stringify(text)}`);
      assertFetches();
    }
    console.log(`PASS DOS ${program}: correct redirected MEMFS output; first-use fetch and cached rerun`);
  }
  for (const module of msdosModules)
    assert.equal(moduleFetches.filter(request => request.name === module).length, 1,
      `${module} was downloaded exactly once across its real first and cached runs`);
}

async function checkKermit() {
  stage = 'Kermit BBS.TAK association and lazy module';
  assert.equal(bbs.calls.length, 0, 'no WebSocket opens before a Hayes dial');
  assert.deepEqual(demoBytes('KERMIT.EXE'),
    readFileSync(new URL('../build/kermit/KERMIT.EXE', import.meta.url)),
    'H: publishes the complete source-built DOS executable');
  assert.deepEqual(demoBytes('BBS.TAK'),
    readFileSync(new URL('../data/BBS.TAK', import.meta.url)), 'the shipped TAKE file is installed');
  send('\x1b[H');
  await until('home selection before BBS.TAK', text => /\.\.|B30\.BAT|BBS|GAMES/.test(selected(text)));
  await selectFile('BBS.TAK');
  expectedFetches.push('kermit.wasm');
  send('\r');
  await releaseProgramFetch('kermit.wasm');
  await until('Kermit renders the captured ENiGMA / BBS version banner', () => sawBBSBanner);
  assert.equal(bbs.calls.length, 1, 'ATDT opened exactly one WebSocket');
  const call = bbs.calls[0];
  assert.deepEqual(Uint8Array.from(call.replies), fixtureReplies,
    'the shared C modem answers the captured telnet negotiation exactly');
  await until('the fake BBS finishes its fixture replay', text => text.includes('TEST BBS READY'));
  console.log('PASS Kermit dial: Enter on BBS.TAK lazy-loads Kermit and renders ENiGMA / BBS version');

  stage = 'Kermit bidirectional terminal data';
  send('hello-bbs\r');
  await until('typed text reaches the fake BBS and its echo reaches Kermit', text =>
    call.text.includes('hello-bbs\r') && text.includes('BBS ECHO: hello-bbs'));
  console.log('PASS Kermit data: typed text traverses UART, modem and binary WebSocket in both directions');

  stage = 'Kermit guarded Hayes escape and hangup';
  // These waits are part of the Hayes protocol, not guesses about UI state.
  await new Promise(done => realSetTimeout(done, 1100));
  send('+++');
  await until('the one-second post-escape guard gives OK', text => /\bOK\b/.test(text));
  send('ATH\r');
  await until('ATH prints NO CARRIER and closes the transport', text =>
    text.includes('NO CARRIER') && call.closed);
  assert.ok(!call.text.includes('+++') && !call.text.includes('ATH'),
    'escape and hangup commands must not leak to the BBS');
  console.log('PASS Kermit hangup: guarded +++, ATH, NO CARRIER and WebSocket close');

  stage = 'Kermit EXIT restores VC panels';
  send('\x1d');
  await until('Kermit offers its single-character escape commands', text => text.includes('Command>'));
  send('C');
  await until('Ctrl-] then C returns to the Kermit command prompt', text => text.includes('MS-Kermit>'));
  send('EXIT\r');
  await until('EXIT returns to VC with both panels redrawn', isPanel);
  assert.equal(exitCalls, 0, 'Kermit exits only its DOS child');
  assertFetches();
  console.log('PASS Kermit exit: Ctrl-] C, EXIT restores VC panels');
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
  const firstLine = demoBytes('README.TXT').toString('utf8').split(/\r?\n/)[0];
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
  const firstLine = demoBytes('README.TXT').toString('utf8').split(/\r?\n/)[0];
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
  if (vc?._vc_source_snapshot) {
    try {
      const snapshot = takeSourceSnapshot();
      console.error(`Guest location: ${JSON.stringify({ raw: snapshot.raw,
        current: snapshot.current, recent: snapshot.recent?.slice(0, 8) })}`);
    } catch { /* A fatal runtime trap may also prevent inspection. */ }
  }
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
  if (fetchFailure || fetchTimeout || fileFailure) {
    // Exercise the browser's real fetch/status/arrayBuffer branch entirely
    // in memory. No listener, HTTP server, or network request is involved.
    globalThis.fetch = async (url, options) => {
      if (new URL(url).pathname.endsWith('.bin')) {
        const { request, resource } = await fileRequest(url);
        const failed = fileFailure && request.name === lazyAssetName(demoBytes('SRC/VC.ASM'))
          && fileFetches.filter(item => item.name === request.name).length === 1;
        return { ok: !failed, status: failed ? 503 : 200, async arrayBuffer() {
          request.bodyReads++;
          const bytes = await programBytes(resource);
          return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
        } };
      }
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
  if (!fetchFailure && !fetchTimeout && !memoryLimit && !msdosOnly && !hackOnly && !sourceOnly && !lazyParentMove && !lazyRmdir) await installSourcePanel();
  const wasmBinary = await readFile(new URL('./vc.wasm', moduleURL));
  const installedAfter = Date.now();
  await createVC({
    wasmBinary,
    // Match main's declared minimum, but prohibit all growth. The pressure
    // and fragmentation scenarios then consume real allocations in this cap.
    ...(memoryLimit ? { wasmMemory: new WebAssembly.Memory({ initial: 1536, maximum: 1536 }) } : {}),
    instantiateWasm(imports, receiveInstance) {
      const compiled = new WebAssembly.Module(wasmBinary);
      const instance = new WebAssembly.Instance(compiled, imports);
      wasmExports = instance.exports;
      return receiveInstance(instance, compiled);
    },
    locateFile: (name) => new URL(`${name}?v=${buildHash}`, moduleURL).href,
    vcFetchProgram: fetchFailure || fetchTimeout || fileFailure ? undefined : fetchProgramBytes,
    vcModem: modem,
    ...(fetchTimeout ? { vcProgramFetchTimeoutMs: 100 } : {}),
    preRun: [(module) => {
      vc = module;
      module.ENV.VC_SCREEN_DUMP = screenPath;
      module.ENV.VC_FRAME_DUMP = framePath;
      if (fileFailure) module.ENV.VC_TRACE = '1';
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
  assert.equal(fileFetches.length, 0, 'VC reaches its first screen before any lazy file contents are fetched');
  assert.equal(sourceFetches.length, 0, 'no source index, maps or source text load before the first VC screen');
  startupHeapBytes = vc.HEAPU8.byteLength;
  startupHeapTop = wasmExports.sbrk(0);
  assert.equal(vc.FS.cwd(), '/home/vc');
  assert.ok(!screen.includes('.config') && !screen.includes('.cache'),
    'VC keeps its settings and log off the H: demo drive');
  assert.ok(vc.FS.stat('/home/vc/GWBASIC.EXE').size > 50000,
    'GW-BASIC is an actual MZ file on H:');
  const installedBefore = Date.now();
  for (const name of readdirSync(demoDirectory, { recursive: true })) {
    const expected = statSync(join(demoDirectory, name));
    const installed = vc.FS.stat(`/home/vc/${name}`);
    if (expected.isFile()) {
      assert.equal(installed.size, expected.size, `${name}: listing has its real size without content`);
      const modified = installed.mtime.getTime();
      assert.ok(modified >= installedAfter && modified <= installedBefore,
        `${name}: listing retains the eager installer's real startup date`);
    } else assert.ok(vc.FS.isDir(installed.mode), `${name}: complete directory tree at first screen`);
  }
  assert.ok(!vc.FS.analyzePath('/home/vc/GAMES/NOTHING.TXT').exists);
  console.log('PASS 1: startup shows 10Quit and README on H:');

  if (fileFailure || lazyFilesOnly) {
    await checkLazyFiles();
    await quitVC();
    console.log('web lazy-file smoke passed');
    exitAfterOutput(0);
    await new Promise(() => {});
  }
  if (vc405Only) {
    await checkVC405();
    await quitVC();
    console.log('web VC405 smoke passed');
    exitAfterOutput(0);
    await new Promise(() => {});
  }

  if (msdosOnly) {
    await checkCommand();
    await checkDosUtilities();
    await quitVC();
    console.log('web DOS smoke: real DIR, batch files, prompt, six utilities, exact first-use and cached module fetches passed');
    exitAfterOutput(0);
    await new Promise(() => {});
  }

  if (kermitOnly) {
    await checkKermit();
    await quitVC();
    console.log('web Kermit smoke: dial, captured ANSI, duplex typing, guarded hangup and VC return passed');
    exitAfterOutput(0);
    await new Promise(() => {});
  }

  if (fetchFailure || fetchTimeout || memoryLimit) {
    if (fetchFailure) await checkFetchFailures();
    else if (fetchTimeout) await checkStalledDownloads();
    else if (memoryLimit) await checkMemoryLimit();
    exitAfterOutput(0);
    await new Promise(() => {});
  }

  if (sourceOnly) {
    await checkLazySources();
    await quitVC();
    console.log('web H: file smoke: unopened directory move and exact cached sources passed');
    exitAfterOutput(0);
    await new Promise(() => {});
  }
  if (lazyParentMove || lazyRmdir) {
    await checkLazyDirectoryOperation();
    await quitVC();
    exitAfterOutput(0);
    await new Promise(() => {});
  }
  if (hackOnly) {
    await checkHack();
    await quitVC();
    console.log('web Hack smoke: lazy files/code, exact assets, Enter and typed launches, status, quit and cache passed');
    exitAfterOutput(0);
    await new Promise(() => {});
  }
  await checkLazyFiles();
  await checkHack();
  await checkLazySources();

  stage = 'Source VC, F9 and unchanged screen';
  const beforeSource = screen;
  let sourceView = await openSource(['VC.COM', 'VC.OVL']);
  assertVCSource(sourceView);
  assert.equal(screen, beforeSource, 'opening Source does not change VC or its key bar');
  assert.ok(sourceFetches.length > 1, 'opening Source fetches its maps and original source text lazily');
  const openedSources = sourceFetches.filter(({ url }) => new URL(url).pathname.endsWith('.txt'));
  console.log(`PASS Source VC open: ${openedSources.length} original text files, ${openedSources.reduce((sum, item) => sum + item.bytes, 0)} bytes; ${sourceFetches.reduce((sum, item) => sum + item.bytes, 0)} bytes including maps/index`);
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
  const firstLine = demoBytes('README.TXT').toString('utf8')
    .split(/\r?\n/)[0];
  assert.ok(firstLine.length, 'the demo README must not be empty');
  send('\x1bOR');
  await until('the README first line in F3', (text) => text.includes(firstLine));
  console.log(`PASS 2: F3 displays ${JSON.stringify(firstLine)}`);
  send('\x1b');
  await until('the panels after closing F3', isPanel);

  stage = '2b Russian F3 viewer';
  const russianBytes = demoBytes('ПРОЧТИ.TXT');
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
  assert.deepEqual(Buffer.from(vc.FS.readFile('/home/vc/ПРОЧТИ.TXT')), russianBytes,
    'the real DOS open materializes the exact CP866 bytes');
  console.log('PASS Russian: F3 displays H:\\ПРОЧТИ.TXT correctly from its CP866 bytes');
  send('\x1b');
  await until('the panels after the Russian viewer', isPanel);

  await checkCommand();

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
  assert.match(demoBytes('BOOTLOGO.TXT').toString('utf8'), /TO FLOWER REPEAT 4 \[PETAL LT 50\] END/);
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
  const spiral = demoBytes('GAMES/SPIRAL.BAS').toString('utf8');
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
  const rogueImage = demoBytes('GAMES/ROGUE.EXE');
  assert.equal(String.fromCharCode(...rogueImage.slice(0, 2)), 'MZ',
    'ROGUE.EXE is a real compiled DOS image in H:\\GAMES');
  assert.ok(rogueImage.length > 50000, 'the game must contain the linked Rogue/PDCurses program');
  assert.ok(!vc.FS.analyzePath('/home/vc/ROGUE.EXE').exists,
    'DOS PATH finds H:\\GAMES without a second root copy');
  assert.match(demoBytes('GAMES/ROGUELIC.TXT').toString('utf8'),
    /Michael Toy, Ken Arnold and Glenn Wichman/);
  assert.match(demoBytes('GAMES/PDCLIC.TXT').toString('utf8'), /public domain/);
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
  assert.deepEqual(demoBytes('VZ.COM'),
    readFileSync(new URL('../third_party/vzeditor/VZ-IBM/US/VZUS.COM', import.meta.url)),
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
    assert.deepEqual(demoBytes(installed), definition,
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
  assert.deepEqual(expectedFetches, ['hack.wasm', 'command.wasm', ...legacyModules.slice(0, 4)],
    'Hack, COMMAND and the first four legacy programs fetched exactly once');
  console.log('PASS 25: typed vz NEW.TXT creates and saves a real H: file');

  await checkKermit();
  await checkDosUtilities();
  await checkVC405();
  assert.deepEqual([...expectedFetches].sort(), [...programModules].sort(),
    'all fifteen programs fetched exactly once');
  for (const name of ['GWBASIC.EXE', 'BOOTLOGO.COM', 'GAMES/ROGUE.EXE', 'GAMES/HACK/HACK.EXE', 'VZ.COM', 'KERMIT.EXE',
    'COMMAND.COM', ...msdosFiles.slice(1).map(name => `DOS/${name}`), 'VC405/VC.COM', 'VC405/VCSETUP.COM']) {
    assert.deepEqual(Buffer.from(vc.FS.readFile(`/home/vc/${name}`)), demoBytes(name),
      `${name}: a real DOS open preserved every published executable byte`);
  }

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
    'INITIAL_MEMORY must cover loading all fifteen programs without heap growth');
  // The measured peak depends on load order: emmalloc asks sbrk for a whole
  // new block when no free block fits. Bound every order: each load's guard
  // may take a fresh 2N + 64 KiB above the startup heap top.
  const sideBytes = [...programFiles.values()].reduce((sum, name) =>
    sum + statSync(join(dirname(modulePath), name)).size, 0);
  const fileAssets = readdirSync(dirname(modulePath)).filter(name => /^file\.[0-9a-f]{12}\.bin$/.test(name));
  const fileBytes = fileAssets.reduce((sum, name) => sum + statSync(join(dirname(modulePath), name)).size, 0);
  // References now allocate after startup. Bound an original, a DOS candidate
  // and the optional no-WebCrypto SHA scratch copy per asset, plus stream/
  // allocator bookkeeping, even if no freed block is reused. This includes
  // unopened guides/sources and ordinary-HTTP fallback load orders too.
  const fileAllowance = 3 * fileBytes + fileAssets.length * 4096;
  const worst = startupHeapTop + 2 * sideBytes + programFiles.size * 64 * 1024 + fileAllowance;
  assert.ok(startupHeapBytes - worst >= Math.max(4 * 1024 * 1024, worst * 0.2),
    `INITIAL_MEMORY leaves at least 4 MiB and 20% headroom above the any-order bound ${worst}`);
  console.log(`PASS memory: any-order bound ${worst} bytes including ${fileAllowance} lazy-file allowance; headroom ${startupHeapBytes - worst} bytes`);

  assert.equal(new Set(sourceFetches.map(({ url }) => url)).size, sourceFetches.length,
    'reopening Source reuses every previously fetched map and original source file');
  console.log(`PASS Source cache: ${sourceFetches.length} immutable lazy assets, ${sourceFetches.reduce((sum, item) => sum + item.bytes, 0)} fetched bytes`);
  sourcePanel.dispose();

  stage = '26 quit and exit hook';
  await quitVC();
  console.log('PASS 26: F10, Enter quits and fires the exit hook');
  assertFetches();
  console.log('web smoke: legacy checks, Hack, DOS shell/utilities, Russian F3, lazy H: files, VC405, Source, and all fifteen first-use/cached module fetches passed');
  exitAfterOutput(0);
} catch (error) {
  fail(error);
}
