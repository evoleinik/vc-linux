// The real translated VC in Emscripten's MEMFS, with the same byte hooks as
// the page. Actions wait for screen state; timers only put a bound on failure.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
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

const modulePath = process.argv[2]
  ? resolve(process.argv[2])
  : fileURLToPath(new URL('../build/web/vc.mjs', import.meta.url));
const { default: createVC } = await import(pathToFileURL(modulePath));
const screenPath = '/tmp/vc-screen.txt';
const encoder = new TextEncoder();
const observers = new Set();
let vc;
let screen = '';
let pending = new Uint8Array();
let inputReads = 0;
let exitCode;
let exitCalls = 0;
let stage = '1 startup';
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
    const deadline = setTimeout(() => {
      finish(new Error(`Timed out waiting for ${description}`));
    }, timeout);
    observers.add(check);
    observe();
  });
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

function fail(error) {
  let log = '';
  try {
    screen = vc.FS.readFile(screenPath, { encoding: 'utf8' });
    log = vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' });
  } catch { /* A startup failure may precede the screen/log. */ }
  console.error(`FAIL [${stage}]: ${error?.stack || error}\n${screen}\n--- VC log ---\n${log}`);
  console.error(`Speaker events in current stage: ${JSON.stringify(speakerEvents)}`);
  exitAfterOutput(1);
}

// An Asyncify rewind runs from a timer, outside the async test's try/catch.
// Preserve the same stage/screen/log diagnostics for guest runtime traps.
process.on('uncaughtException', fail);
process.on('unhandledRejection', fail);

try {
  await createVC({
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
    print: (text) => console.log(`[VC stdout] ${text}`),
    printErr: (text) => console.error(`[VC stderr] ${text}`),
  });

  await until('10Quit and README in the H: listing', (text) =>
    isPanel(text) && text.includes('H:\\'));
  assert.equal(vc.FS.cwd(), '/home/vc');
  assert.ok(!screen.includes('.config') && !screen.includes('.cache'),
    'VC keeps its settings and log off the H: demo drive');
  assert.ok(vc.FS.stat('/home/vc/GWBASIC.EXE').size > 50000,
    'GW-BASIC is an actual MZ file on H:');
  assert.ok(!vc.FS.analyzePath('/home/vc/GAMES/NOTHING.TXT').exists);
  console.log('PASS 1: startup shows 10Quit and README on H:');

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
  send('gwbasic\r');
  await until('GW-BASIC banner and Ok from its own code', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));
  console.log('PASS 5: DOS PATH/EXEC starts H:\\GWBASIC.EXE and shows Ok');

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
  console.log('PASS 11: untranslated CALL stops only GW-BASIC; VC and a second BASIC still work');

  stage = '12 bootLogo command';
  assert.equal(vc.FS.stat('/home/vc/BOOTLOGO.COM').size, 503, 'BOOTLOGO.COM is the original NASM COM file on H:');
  assert.ok(!vc.FS.analyzePath('/home/vc/LOGO.COM').exists, 'the old LOGO.COM alias is not installed');
  assert.ok(!vc.FS.analyzePath('/home/vc/LOGO.TXT').exists, 'the guide is installed under its own name');
  assert.match(vc.FS.readFile('/home/vc/BOOTLOGO.TXT', { encoding: 'utf8' }), /TO FLOWER REPEAT 4 \[PETAL LT 50\] END/);
  send('bootlogo\r');
  await until('bootLogo prompt in the graphics canvas', () =>
    graphics?.mode === 4 && graphicsText(graphics).split('\n')[21].trimEnd() === '>');
  assert.equal(graphics.width, 320);
  assert.equal(graphics.height, 200);
  assert.equal(graphics.pixels.length, 64000);
  assert.deepEqual([...graphics.palette], [0, 0, 0, 85, 255, 255, 255, 85, 255, 255, 255, 255]);
  console.log('PASS 12: bootlogo starts the real H:\\BOOTLOGO.COM with a CGA canvas frame');

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
  send('rogue\r');
  await until('Rogue dungeon and Level: 1 Gold: Hp: status', (text) =>
    isRogue(text) && rogueStatus.exec(text)[1] === '1');
  assert.equal(graphics, null, 'PDCurses uses the real DOS text screen');
  console.log('PASS 19: rogue resolves the real H:\\GAMES\\ROGUE.EXE and shows a level-1 dungeon');

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
  send('\x1bOS');
  await until('VZ showing the original README first line after F4', (text) =>
    !hasPanels(text) && text.includes(firstLine) && text.includes('File'));
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
  console.log('PASS 25: typed vz NEW.TXT creates and saves a real H: file');

  stage = '26 quit and exit hook';
  send('\x1b[21~');
  await until('the quit confirmation', (text) =>
    text.includes('Do you want to quit the Volkov Commander?') && text.includes('Yes'));
  send('\r');
  await until('the JavaScript exit hook', () => exitCalls > 0);
  assert.equal(exitCalls, 1, 'the exit hook fires exactly once');
  assert.equal(exitCode, 0, 'VC exits successfully');
  console.log('PASS 26: F10, Enter quits and fires the exit hook');
  console.log('web smoke: all 26 checks passed');
  exitAfterOutput(0);
} catch (error) {
  fail(error);
}
