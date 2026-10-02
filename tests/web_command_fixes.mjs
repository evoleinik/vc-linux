// COMMAND review regressions against the real browser wasm, offline in Node.
// Each case gets a fresh worker/MEMFS; the outer watchdog also catches a guest
// that never yields. Optional case names make the original failures repeatable.
// node tests/web_command_fixes.mjs build/web/vc.mjs [case ...]
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { isMainThread, parentPort, Worker, workerData } from 'node:worker_threads';

const cases = ['vc', 'vc-line', 'kermit', 'gwbasic', 'debug', 'tab', 'con', 'nul',
  'pipe-h', 'pipe-c', 'fcb-ren', 'fcb-del'];
// Hack's PATH entry crosses a DOS environment-allocation paragraph. Exercise
// both sides of several boundaries, not just one lucky transient-code base.
const pathPadding = [0, 13, 14, 29, 30, 45, 46, 61, 62, 64];
cases.push(...pathPadding.map(size => `vc-path-${size}`));
const timeout = Number(process.env.WEB_SMOKE_TIMEOUT || 12000);

if (isMainThread) {
  const modulePath = resolve(process.argv[2] || 'build/web/vc.mjs');
  const selected = process.argv.slice(3);
  let failed = 0;
  for (const scenario of selected.length ? selected : cases) {
    assert.ok(cases.includes(scenario), `unknown case: ${scenario}`);
    const code = await new Promise(done => {
      const worker = new Worker(new URL(import.meta.url), { workerData: { modulePath, scenario } });
      let timer;
      function arm(stage) {
        clearTimeout(timer);
        timer = setTimeout(() => {
          console.error(`FAIL COMMAND ${scenario}: stalled at ${stage}`);
          worker.terminate().then(() => done(1));
        }, timeout + 1000);
      }
      arm('startup');
      worker.on('message', arm);
      worker.on('error', error => { console.error(error); });
      worker.on('exit', result => { clearTimeout(timer); done(result); });
    });
    if (code) ++failed;
  }
  assert.equal(failed, 0, `${failed} COMMAND browser regression(s) failed`);
  console.log('PASS COMMAND browser regressions');
  process.exit(0);
}

const { modulePath, scenario } = workerData;
const nestedVC = scenario === 'vc' || scenario.startsWith('vc-path-');
const moduleURL = pathToFileURL(modulePath);
const { default: createVC } = await import(moduleURL);
const screenPath = '/tmp/brief31-screen.txt';
const encoder = new TextEncoder();
const observers = new Set();
const fetches = new Map();
let vc;
let screen = '';
let pending = new Uint8Array();
let exitCode;
let stage = 'startup';
let outcome = 'DOS operation completes';

function observe() {
  if (vc) {
    try { screen = vc.FS.readFile(screenPath, { encoding: 'utf8' }); }
    catch (error) { if (error.errno !== 44 && error.code !== 'ENOENT') throw error; }
  }
  for (const check of [...observers]) check();
}

function send(text) {
  const bytes = encoder.encode(text);
  const next = new Uint8Array(pending.length + bytes.length);
  next.set(pending);
  next.set(bytes, pending.length);
  pending = next;
}

function until(description, predicate) {
  stage = description;
  parentPort.postMessage(description);
  return new Promise((done, reject) => {
    function finish(error) {
      clearTimeout(timer);
      observers.delete(check);
      if (error) reject(error);
      else done(screen);
    }
    function check() {
      if (exitCode !== undefined) finish(new Error(`VC exited ${exitCode}`));
      else if (predicate(screen)) finish();
    }
    const timer = setTimeout(() => finish(new Error(`timeout at ${description}`)), timeout);
    observers.add(check);
    observe();
  });
}

const panels = text => text.startsWith('╔') && (text.split('\n')[24] || '').includes('10Quit');
const lastLine = text => text.split('\n').filter(line => line.trim()).at(-1)?.trimEnd() || '';
const prompt = text => !panels(text) && /^[CH]:\\[^\n>]*>\s*$/m.test(text);
const kermitPrompt = text => /^MS-Kermit>\s*$/.test(lastLine(text));
// BASIC reserves row 25 for its function-key labels. Only accept its current
// prompt above that row, not an old Ok still visible behind a nested shell.
const basicPrompt = text => /^Ok\s*$/.test(lastLine(text.split('\n').slice(0, 24).join('\n')));
const debugPrompt = text => /^-\s*$/.test(lastLine(text));
const log = () => vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' });
const loads = name => log().split('\n').filter(line => line.includes(`translation ${name}`)).length;
const terminations = () => log().split('\n').filter(line => line.includes('terminate psp ')).length;
const exists = path => vc.FS.analyzePath(path).exists;
const bytes = path => Buffer.from(vc.FS.readFile(path));

async function commandLine(command) {
  const marker = `>${command}`;
  send(`${command}\r`);
  await until(`COMMAND completes ${command}`, text => {
    const after = text.slice(text.lastIndexOf(marker) + marker.length);
    return text.includes(marker) && pending.length === 0 && prompt(after);
  });
  return screen.slice(screen.lastIndexOf(marker) + marker.length);
}

async function startCommand() {
  const before = loads('COMMAND.COM');
  send('command\r');
  await until('COMMAND prompt', text => loads('COMMAND.COM') > before && prompt(text));
}

async function quitChildVC(returned) {
  send('\x1b[21~');
  await until('nested VC quit confirmation', text =>
    text.includes('Do you want to quit the Volkov Commander?') && text.includes('Yes'));
  const before = terminations();
  send('\r');
  await until('nested VC returns to its caller', text => pending.length === 0 &&
    terminations() > before && !text.includes('Do you want to quit the Volkov Commander?') && returned(text));
}

async function finishCommand() {
  const output = await commandLine(`echo alive-${scenario}`);
  assert.match(output, new RegExp(`^alive-${scenario}\\s*$`, 'm'));
  send('exit\r');
  await until('parent VC panels survive', panels);
  assert.equal(exitCode, undefined);
}

try {
  await createVC({
    arguments: [], wasmBinary: await readFile(new URL('./vc.wasm', moduleURL)),
    locateFile: name => new URL(name, moduleURL).href,
    preRun: [module => { vc = module; module.ENV.VC_SCREEN_DUMP = screenPath; }],
    vcReadInput(capacity) {
      observe();
      const result = pending.slice(0, capacity);
      pending = pending.subarray(result.length);
      return result;
    },
    vcOutput: observe,
    vcExit(code) { exitCode = code; observe(); },
    async vcFetchProgram(url) {
      const resource = new URL(url);
      assert.equal(resource.protocol, 'file:', 'regressions never use the network');
      const name = resource.pathname.split('/').at(-1);
      fetches.set(name, (fetches.get(name) || 0) + 1);
      return new Uint8Array(await readFile(resource));
    },
    print: text => console.log(`[VC] ${text}`),
    printErr: text => console.error(`[VC] ${text}`),
  });
  await until('initial VC panels', panels);
  assert.equal(fetches.size, 0, 'first screen does not fetch COMMAND or utilities');

  if (scenario !== 'vc-line') await startCommand();
  if (scenario.startsWith('vc-path-')) {
    const padding = Number(scenario.slice('vc-path-'.length));
    const base = 'H:\\;H:\\DOS;H:\\GAMES;C:\\var\\vc\\config\\vc-linux\\;C:\\';
    // The extra entry is deliberately nonexistent and searched only after
    // VC's real installation directory. Only the copied environment length
    // changes; neither program bytes nor the translated loader are patched.
    const command = `set PATH=${base}${padding ? ';' + 'P'.repeat(padding - 1) : ''}`;
    assert.ok(command.length < 127, 'PATH sweep fits the original DOS command-line buffer');
    send(`${command}\r`);
    await until(`COMMAND applies ${padding} bytes of PATH padding`, text =>
      pending.length === 0 && /^[CH]:\\[^\n>]*>\s*$/.test(lastLine(text)) &&
      text.split('\n').map(line => line.trimEnd()).join('').includes(`>${command}`));
  }
  if (nestedVC || scenario === 'vc-line') {
    const before = loads('VC.OVL');
    const beforeLog = log().length;
    send('vc\r');
    await until('VC launched under COMMAND or reports a DOS overlay error', text =>
      (loads('VC.OVL') > before && panels(text)) ||
      (nestedVC && prompt(text) && text.includes('Error reading overlay file.')));
    const childLog = log().slice(beforeLog);
    assert.match(childLog, /translation VC\.COM \(DOS-hosted loader\)/);
    if (/^terminate psp [0-9A-F]{4} code 2$/m.test(childLog)) {
      outcome = 'VC reports DOS overlay error 2';
      // DOS 2's loader has no DOS 3 executable-path environment trailer.
      // VC can reject its missing overlay path; the brief permits DOS errors,
      // but never a runtime fatal or losing the enclosing shell/file manager.
      // With /C vc, COMMAND exits too and the OUTER VC redraws its panels.
      // Those panels must not be mistaken for a successfully nested VC.
      if (nestedVC) assert.match(screen, /Error reading overlay file\./);
      else assert.ok(panels(screen));
      assert.doesNotMatch(childLog, /fatal:|no translated code at/);
    } else {
      assert.ok(panels(screen), 'successful nested VC must display its panels');
      await quitChildVC(nestedVC ? prompt : panels);
    }
  } else if (scenario === 'kermit') {
    outcome = 'Kermit PUSH reports a guest error';
    send('kermit\r');
    await until('Kermit prompt under COMMAND', kermitPrompt);
    const before = loads('COMMAND.COM');
    send('push\r');
    await until('Kermit PUSH shell or DOS error', text => (loads('COMMAND.COM') > before && prompt(text)) ||
      (kermitPrompt(text) && /(?:not enough memory|cannot|unable|error)/i.test(text)));
    if (prompt(screen)) {
      assert.match(await commandLine('echo push-alive'), /^push-alive\s*$/m);
      const before = terminations();
      send('exit\r');
      await until('PUSH returns to Kermit', text => pending.length === 0 &&
        terminations() > before && kermitPrompt(text));
      outcome = 'Kermit PUSH runs COMMAND, echoes, and returns';
    }
    send('exit\r');
    await until('Kermit returns to COMMAND', prompt);
  } else if (scenario === 'gwbasic') {
    outcome = 'BASIC SHELL reports a guest error';
    send('gwbasic\r');
    await until('BASIC prompt under COMMAND', basicPrompt);
    const before = loads('COMMAND.COM');
    send('SHELL\r');
    await until('BASIC SHELL or DOS error', text => (loads('COMMAND.COM') > before && prompt(text)) ||
      (basicPrompt(text) && /(?:memory|error|unavailable)/i.test(text)));
    if (prompt(screen)) {
      assert.match(await commandLine('echo basic-alive'), /^basic-alive\s*$/m);
      const before = terminations();
      send('exit\r');
      await until('SHELL returns to BASIC', text => pending.length === 0 &&
        terminations() > before && basicPrompt(text));
      outcome = 'BASIC SHELL runs COMMAND, echoes, and returns';
    }
    send('SYSTEM\r');
    await until('BASIC returns to COMMAND', prompt);
  } else if (scenario === 'debug') {
    outcome = 'DEBUG reports a contained guest error';
    // Not a preserved image: DEBUG may load it, but G must not run a host
    // program or let an untranslated address take down the enclosing VC.
    vc.FS.writeFile('/home/vc/X.COM', new Uint8Array([0xb8, 0x00, 0x4c, 0xcd, 0x21]));
    send('debug X.COM\r');
    await until('DEBUG loads X.COM or reports a DOS error', text => debugPrompt(text) ||
      (prompt(text) && /(?:error|not found|cannot)/i.test(text)));
    if (debugPrompt(screen)) {
      send('g\r');
      await until('DEBUG G is contained', text => prompt(text) ||
        (debugPrompt(text) && /(?:terminated|error|No translated code)/i.test(text)));
      if (debugPrompt(screen)) { send('q\r'); await until('DEBUG returns to COMMAND', prompt); }
    }
    assert.match(screen + log(), /(?:No translated code|DOS command error|terminated|error)/i);
  } else if (scenario === 'tab') {
    vc.FS.writeFile('/home/vc/TAB.TXT', 'A\tB\r\n');
    assert.match(await commandLine('type TAB.TXT'), /^A       B\s*$/m,
      'DOS TAB expands to the next eight-column stop');
    assert.doesNotMatch(await commandLine('dir /w'), /○/, 'wide DIR must not display CP437 TAB glyphs');
  } else if (scenario === 'con') {
    send('copy con NOTE.TXT\r');
    await until('COPY CON consumes its command line', text =>
      text.includes('copy con NOTE.TXT') && pending.length === 0);
    send('first typed line\r');
    await until('CON echoes the first line', text => text.includes('first typed line'));
    send('second typed line\r');
    await until('CON echoes the second line', text => text.includes('second typed line'));
    send('\x1a\r');
    await until('Ctrl-Z completes COPY CON', text => prompt(text) && pending.length === 0);
    assert.ok(exists('/home/vc/NOTE.TXT'), 'COPY CON must create the requested file');
    assert.match(bytes('/home/vc/NOTE.TXT').toString('latin1'),
      /^first typed line\r\nsecond typed line\r\n\x1a?$/,
      'CON handle reads retain both typed lines up to DOS EOF');
  } else if (scenario === 'nul') {
    vc.FS.writeFile('/home/vc/nul', 'regular file must remain untouched');
    const output = await commandLine('dir > nul');
    assert.equal(bytes('/home/vc/nul').toString(), 'regular file must remain untouched');
    assert.doesNotMatch(output, /File\(s\)|Directory of/, 'NUL discards directory output');
  } else if (scenario.startsWith('pipe-')) {
    const drive = scenario === 'pipe-h' ? 'H' : 'C';
    const directory = drive === 'H' ? '/home/vc' : '/tmp/PIPEWORK';
    if (!exists(directory)) vc.FS.mkdir(directory);
    for (const root of ['/home/vc', '']) {
      for (const name of ['%PIPE1.$$$', '%PIPE2.$$$'])
        vc.FS.writeFile(`${root}/${name}`, `untouched-${root}-${name}`);
    }
    vc.FS.writeFile(`${directory}/ALPHA.TXT`, 'alpha\r\n');
    vc.FS.writeFile(`${directory}/ZETA.TXT`, 'zeta\r\n');
    await commandLine(`${drive}:`);
    if (drive === 'C') await commandLine('cd \\tmp\\PIPEWORK');
    const before = loads('SORT.EXE');
    await commandLine('dir | sort > DIRSORT.TXT');
    assert.ok(loads('SORT.EXE') > before, 'pipeline must execute the DOS SORT image');
    const output = bytes(`${directory}/DIRSORT.TXT`).toString();
    assert.match(output, /ALPHA\s+TXT/);
    assert.match(output, /ZETA\s+TXT/);
    for (const root of ['/home/vc', '']) {
      for (const name of ['%PIPE1.$$$', '%PIPE2.$$$']) {
        assert.ok(exists(`${root}/${name}`), 'pipe scratch must not delete a visible root sentinel');
        assert.equal(bytes(`${root}/${name}`).toString(), `untouched-${root}-${name}`,
          'COMMAND pipe scratch must not touch either visible drive root');
      }
    }
  } else if (scenario === 'fcb-ren' || scenario === 'fcb-del') {
    for (const name of ['A.TXT', 'A1.TXT', 'A2.TXT', 'B1.TXT'])
      vc.FS.writeFile(`/home/vc/${name}`, name);
    if (scenario === 'fcb-ren') {
      await commandLine('ren A?.TXT R?.DAT');
      for (const suffix of ['', '1', '2']) {
        assert.ok(exists(`/home/vc/R${suffix}.DAT`), 'FCB wildcard REN creates each substituted name');
        assert.equal(bytes(`/home/vc/R${suffix}.DAT`).toString(), `A${suffix}.TXT`);
        assert.ok(!exists(`/home/vc/A${suffix}.TXT`));
      }
    } else {
      await commandLine('del A?.TXT');
      for (const suffix of ['', '1', '2']) assert.ok(!exists(`/home/vc/A${suffix}.TXT`));
    }
    assert.equal(bytes('/home/vc/B1.TXT').toString(), 'B1.TXT');
  }
  if (scenario !== 'vc-line') await finishCommand();
  else {
    // Prove the returned outer VC still handles input without exiting it.
    send('\x1b[21~');
    await until('outer VC handles F10', text => text.includes('Do you want to quit the Volkov Commander?'));
    send('\x1b');
    await until('outer VC cancels quit and survives', text => pending.length === 0 &&
      panels(text) && !text.includes('Do you want to quit the Volkov Commander?'));
    assert.equal(exitCode, undefined);
  }
  assert.ok([...fetches.values()].every(count => count === 1), 'nested loads retain the lazy cache');
  console.log(`PASS COMMAND ${scenario}: ${outcome}; parent VC survives`);
  process.stdout.write('', () => process.exit(0));
  await new Promise(() => {});
} catch (error) {
  let trace = '';
  try { trace = log().slice(-16000); } catch { /* May fail before runtime setup. */ }
  console.error(`FAIL COMMAND ${scenario} [${stage}]: ${error.stack || error}\n${screen}\n${trace}`);
  process.stderr.write('', () => process.exit(1));
  await new Promise(() => {});
}
