// The real translated VC in Emscripten's MEMFS, with the same byte hooks as
// the page. Actions wait for screen state; timers only put a bound on failure.
import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const modulePath = process.argv[2]
  ? resolve(process.argv[2])
  : fileURLToPath(new URL('../build/web/vc.mjs', import.meta.url));
const { default: createVC } = await import(pathToFileURL(modulePath));
const screenPath = '/tmp/vc-screen.txt';
const timeout = Number(process.env.WEB_SMOKE_TIMEOUT || 10000);
const encoder = new TextEncoder();
const observers = new Set();
let vc;
let screen = '';
let pending = new Uint8Array();
let exitCode;
let exitCalls = 0;
let stage = '1 startup';

function observe() {
  if (vc) {
    try {
      screen = vc.FS.readFile(screenPath, { encoding: 'utf8' });
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
    const deadline = setTimeout(() => {
      finish(new Error(`Timed out waiting for ${description}`));
    }, timeout);
    observers.add(check);
    observe();
  });
}

const isPanel = (text) => text.startsWith('╔') && (text.split('\n')[24] || '').includes('5Copy')
  && text.includes('10Quit') && text.includes('README');
// The shipped VC.INI selects the right panel (Active=1); its H: listing is
// independent of the left panel's remembered C: directory.
const selected = (text) => (text.split('\n')[21] || '').slice(40);
const commandLine = (text) => text.split('\n')[23] || '';

async function selectReadme() {
  // The status row, not the listing (where every name is always visible),
  // identifies the active selection. Advance only after the last key landed.
  for (let step = 0; step < 12; step++) {
    const previous = selected(screen);
    if (previous.includes('README.TXT')) return;
    send('\x1b[B');
    await until('the selection to move', (text) => selected(text) !== previous);
  }
  assert.fail('README.TXT was not reachable in the H: panel');
}

try {
  await createVC({
    preRun: [(module) => {
      vc = module;
      module.ENV.VC_SCREEN_DUMP = screenPath;
    }],
    vcReadInput(capacity) {
      observe();
      const bytes = pending.slice(0, capacity);
      pending = pending.subarray(bytes.length);
      return bytes;
    },
    vcOutput() {
      observe();
    },
    vcExit(code) {
      exitCode = code;
      exitCalls++;
      observe();
    },
    print: (text) => console.log(`[VC stdout] ${text}`),
    printErr: (text) => console.error(`[VC stderr] ${text}`),
  });

  await until('10Quit and README in the H: listing', (text) =>
    isPanel(text) && text.includes('H:\\'));
  assert.equal(vc.FS.cwd(), '/home/vc');
  assert.ok(!screen.includes('.config') && !screen.includes('.cache'),
    'VC keeps its settings and log off the H: demo drive');
  console.log('PASS 1: startup shows 10Quit and README on H:');

  stage = '2 F3 viewer';
  await selectReadme();
  const firstLine = vc.FS.readFile('/home/vc/README.TXT', { encoding: 'utf8' })
    .split(/\r?\n/)[0];
  assert.ok(firstLine.length, 'the demo README must not be empty');
  send('\x1bOR');
  await until('the README first line in F3', (text) => text.includes(firstLine));
  console.log(`PASS 2: F3 displays ${JSON.stringify(firstLine)}`);
  send('\x1b');
  await until('the panels after closing F3', isPanel);

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

  stage = '5 quit and exit hook';
  send('\x1b[21~');
  await until('the quit confirmation', (text) =>
    text.includes('Do you want to quit the Volkov Commander?') && text.includes('Yes'));
  send('\r');
  await until('the JavaScript exit hook', () => exitCalls > 0);
  assert.equal(exitCalls, 1, 'the exit hook fires exactly once');
  assert.equal(exitCode, 0, 'VC exits successfully');
  console.log('PASS 5: F10, Enter quits and fires the exit hook');
  console.log('web smoke: all 5 checks passed');
  process.exit(0);
} catch (error) {
  let log = '';
  try {
    log = vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' });
  } catch { /* A startup failure may precede the log. */ }
  console.error(`FAIL [${stage}]: ${error.stack || error}\n${screen}\n--- VC log ---\n${log}`);
  process.exit(1);
}
