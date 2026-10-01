// Busy-loop comparison for the single-wasm main and lazy-module builds.
// node tests/web_benchmark.mjs path/to/vc.mjs
// node tests/web_benchmark.mjs --compare main.log lazy.log
// Run builds sequentially on an otherwise idle machine. Startup, downloads,
// state-driven typing, resets, and result checks are outside every sample.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { isMainThread, parentPort, Worker } from 'node:worker_threads';

const timeout = Number(process.env.WEB_BENCH_TIMEOUT || 60000);
const warmups = Number(process.env.WEB_BENCH_WARMUPS || 2);
const samples = Number(process.env.WEB_BENCH_SAMPLES || 5);
const loop = 'FOR I=1 TO 30000: A=A+I: NEXT';
assert.ok(Number.isInteger(warmups) && warmups >= 1, 'at least one warmup');
assert.ok(Number.isInteger(samples) && samples >= 3, 'at least three measured samples');

function median(values) {
  const ordered = [...values].sort((a, b) => a - b);
  const middle = Math.floor(ordered.length / 2);
  return ordered.length % 2 ? ordered[middle] : (ordered[middle - 1] + ordered[middle]) / 2;
}

function exitAfterOutput(code) {
  process.stdout.write('', () => process.stderr.write('', () => process.exit(code)));
}

if (isMainThread && process.argv[2] === '--compare') {
  try {
    assert.equal(process.argv.length, 5, 'compare takes baseline.log and current.log');
    const results = [];
    for (const path of process.argv.slice(3)) {
      const records = (await readFile(path, 'utf8')).trim().split('\n').filter(line => line.startsWith('{'));
      assert.ok(records.length, `${path}: complete benchmark JSON is required`);
      const result = JSON.parse(records.at(-1));
      assert.equal(result.loop, loop, `${path}: the exact requested busy loop`);
      assert.match(result.wasm_sha256, /^[0-9a-f]{64}$/, `${path}: measured wasm provenance`);
      assert.ok(result.warmup_ms.length >= 1 && result.samples_ms.length >= 3,
        `${path}: warmup and repeated samples are required`);
      assert.ok([...result.warmup_ms, ...result.samples_ms].every(value => Number.isFinite(value) && value > 0),
        `${path}: every time must be finite and positive`);
      assert.equal(result.median_ms, median(result.samples_ms), `${path}: independently checked median`);
      results.push(result);
    }
    const [baseline, current] = results;
    assert.equal(current.node, baseline.node, 'both builds must use the same Node version');
    assert.equal(current.timer, baseline.timer, 'both builds must use the same timing boundary');
    const ratio = current.median_ms / baseline.median_ms;
    console.log(`benchmark: main ${baseline.median_ms.toFixed(3)} ms; current ${current.median_ms.toFixed(3)} ms; `
      + `${((ratio - 1) * 100).toFixed(2)}% change`);
    console.log('PASS benchmark comparison: measurements validated; no performance threshold');
    exitAfterOutput(0);
  } catch (error) {
    console.error(`FAIL benchmark comparison: ${error?.stack || error}`);
    exitAfterOutput(1);
  }
  await new Promise(() => {});
}

if (isMainThread) {
  // A missing Asyncify yield can stall the guest's JS timers, too.
  const worker = new Worker(new URL(import.meta.url), { argv: process.argv.slice(2) });
  let deadline;
  const arm = (description) => {
    clearTimeout(deadline);
    if (process.env.WEB_BENCH_DEBUG) console.error(`benchmark stage: ${description}`);
    deadline = setTimeout(() => {
      console.error(`FAIL benchmark: stalled waiting for ${description}`);
      worker.terminate().then(() => exitAfterOutput(1));
    }, timeout + 1000);
  };
  arm('startup');
  worker.on('message', arm);
  worker.on('error', (error) => { console.error(error); exitAfterOutput(1); });
  worker.on('exit', (code) => { clearTimeout(deadline); exitAfterOutput(code); });
  await new Promise(() => {});
}

const modulePath = resolve(process.argv[2]
  || fileURLToPath(new URL('../build/web/vc.mjs', import.meta.url)));
const moduleURL = pathToFileURL(modulePath);
const { default: createVC } = await import(moduleURL);
const wasmBytes = await readFile(new URL('./vc.wasm', moduleURL));
const wasmHash = createHash('sha256').update(wasmBytes).digest('hex');
const screenPath = '/tmp/vc-benchmark-screen.txt';
const encoder = new TextEncoder();
const observers = new Set();
let vc;
let screen = '';
let pending = new Uint8Array();
let stage = 'startup';
let exitCode;
let loopEnterPending = false;
let started;
let finished;

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
  return new Promise((resolveWait, reject) => {
    const finish = (error) => {
      clearTimeout(deadline);
      observers.delete(check);
      if (error) reject(error);
      else resolveWait();
    };
    const check = () => { if (predicate(screen)) finish(); };
    const deadline = setTimeout(() => finish(new Error(`Timed out: ${description}`)), timeout);
    observers.add(check);
    observe();
  });
}

async function typeLine(command) {
  // BASIC's typeahead buffer is small. Each chunk waits for its actual glyphs.
  const compact = (text) => text.replace(/\s/g, '');
  for (let start = 0; start < command.length; start += 8) {
    send(command.slice(start, start + 8));
    const expected = compact(command.slice(0, start + 8));
    await until(`input echo ${JSON.stringify(expected)}`, (text) => compact(text).includes(expected));
  }
}

function resultAfter(command, text) {
  const at = text.lastIndexOf(command);
  return at < 0 ? '' : text.slice(at + command.length);
}

async function verify(command, output) {
  await typeLine(command);
  send('\r');
  await until(`verified output after ${command}`, (text) =>
    output.test(resultAfter(command, text)));
}

function fail(error) {
  let log = '';
  try { log = vc.FS.readFile('/var/vc/cache/vc-linux/vc.log', { encoding: 'utf8' }); }
  catch { /* Startup can fail before creating the log. */ }
  console.error(`FAIL benchmark [${stage}]: ${error?.stack || error}\n${screen}\n${log}`);
  exitAfterOutput(1);
}
process.on('uncaughtException', fail);
process.on('unhandledRejection', fail);

try {
  await createVC({
    arguments: [],
    wasmBinary: wasmBytes,
    locateFile: (name) => new URL(name, moduleURL).href,
    vcFetchProgram: async (url) => new Uint8Array(await readFile(new URL(url))),
    preRun: [(module) => { vc = module; module.ENV.VC_SCREEN_DUMP = screenPath; }],
    vcReadInput(capacity) {
      observe();
      const bytes = pending.slice(0, capacity);
      pending = pending.subarray(bytes.length);
      if (loopEnterPending && bytes.includes(13)) {
        assert.deepEqual([...bytes], [13], 'timing begins only on the already-echoed loop Enter');
        loopEnterPending = false;
        started = performance.now();
      }
      return bytes;
    },
    vcOutput: observe,
    vcExit(code) { exitCode = code; observe(); },
    print: (text) => console.log(`[VC stdout] ${text}`),
    printErr: (text) => console.error(`[VC stderr] ${text}`),
  });
  const panels = (text) => text.startsWith('╔') && text.includes('10Quit');
  await until('VC panels', panels);
  send('gwbasic\r');
  await until('GW-BASIC startup and lazy load complete', (text) =>
    text.includes('GW-BASIC') && /^Ok\s*$/m.test(text));

  const durations = [];
  for (let run = 0; run < warmups + samples; run++) {
    await typeLine('CLS: A=0');
    send('\r');
    await until('reset A and fresh cleared BASIC prompt', (text) =>
      !text.includes('CLS: A=0') && text.split('\n')[0].trim() === 'Ok');
    await typeLine(loop);
    started = undefined;
    finished = undefined;
    loopEnterPending = true;
    const completed = until(`loop ${run + 1} finishes with a new Ok`, (text) => {
      if (started === undefined || !/\nOk[^\S\n]*(?:\n|$)/.test(resultAfter(loop, text))) return false;
      finished = performance.now();
      return true;
    });
    send('\r');
    await completed;
    assert.ok(finished > started, 'the loop has a positive measured duration');
    assert.equal(exitCode, undefined, 'the guest stays alive during every loop');
    // These checks run after the timer. I proves all 30,000 iterations ran;
    // the broad A interval permits this 1983 BASIC's default single precision.
    await verify('PRINT I', /\n[^\S\n]*30001[^\S\n]*\nOk[^\S\n]*(?:\n|$)/);
    await verify('PRINT A>449900000 AND A<450100000', /\n[^\S\n]*-1[^\S\n]*\nOk[^\S\n]*(?:\n|$)/);
    const ms = finished - started;
    durations.push(ms);
    console.log(`${run < warmups ? 'warmup' : 'sample'} ${run < warmups ? run + 1 : run - warmups + 1}: ${ms.toFixed(3)} ms`);
  }

  await typeLine('SYSTEM');
  send('\r');
  await until('VC panels after all benchmark runs', panels);
  send('\x1b[21~');
  await until('VC quit confirmation', (text) => text.includes('Do you want to quit the Volkov Commander?'));
  send('\r');
  await until('clean VC exit', () => exitCode !== undefined);
  assert.equal(exitCode, 0);
  const measured = durations.slice(warmups);
  console.log(JSON.stringify({
    module: modulePath, wasm_sha256: wasmHash, node: process.version, loop,
    timer: 'loop Enter consumed to first new Ok; excludes loading, typing, resets, verification',
    warmup_ms: durations.slice(0, warmups), samples_ms: measured, median_ms: median(measured),
  }));
  exitAfterOutput(0);
} catch (error) { fail(error); }
