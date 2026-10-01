// Fresh-Node, offline time to VC's first complete panel screen.
// node tests/web_startup.mjs BEFORE/vc.mjs AFTER/vc.mjs
// Each warmup/sample is a new process; alternate builds to limit order bias.
// Includes JS import, local wasm read/compile, MEMFS setup, unpacking and boot.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, open, readFile, rmdir, unlink } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { spawn } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const script = fileURLToPath(import.meta.url);
const sampleMode = process.argv[2] === '--sample';
const timeout = Number(process.env.WEB_STARTUP_TIMEOUT || 30000);

if (sampleMode) {
  const moduleURL = pathToFileURL(resolve(process.argv[3]));
  const screenPath = '/tmp/brief31-startup-screen.txt';
  let vc;
  let ready;
  let firstScreen;
  let preRun;
  let fetches = 0;
  const screenReady = new Promise(done => { ready = done; });
  const started = performance.now();
  function observe() {
    if (!vc || firstScreen !== undefined) return;
    let screen;
    try { screen = vc.FS.readFile(screenPath, { encoding: 'utf8' }); }
    catch (error) {
      if (error.errno !== 44 && error.code !== 'ENOENT') throw error;
      return;
    }
    if (screen.startsWith('╔') && screen.includes('README') &&
        (screen.split('\n')[24] || '').includes('10Quit')) {
      firstScreen = performance.now();
      ready();
    }
  }
  const { default: createVC } = await import(moduleURL);
  const wasm = await readFile(new URL('./vc.wasm', moduleURL));
  await createVC({
    arguments: [], wasmBinary: wasm,
    locateFile: name => new URL(name, moduleURL).href,
    preRun: [module => {
      vc = module;
      preRun = performance.now();
      module.ENV.VC_SCREEN_DUMP = screenPath;
    }],
    vcOutput: observe,
    vcReadInput() { observe(); return new Uint8Array(); },
    vcFetchProgram() { ++fetches; throw new Error('startup must not fetch side modules'); },
    vcExit(code) { throw new Error(`VC exited before measurement: ${code}`); },
    print: text => process.stderr.write(`${text}\n`),
    printErr: text => process.stderr.write(`${text}\n`),
  });
  observe();
  await screenReady;
  assert.equal(fetches, 0);
  assert.ok(firstScreen > preRun && preRun > started);
  const result = {
    node: process.version,
    wasm_sha256: createHash('sha256').update(wasm).digest('hex'),
    wasm_bytes: wasm.length,
    first_screen_ms: firstScreen - started,
    runtime_ms: firstScreen - preRun,
    lazy_fetches: fetches,
  };
  process.stdout.write(`${JSON.stringify(result)}\n`, () => process.exit(0));
  await new Promise(() => {});
}

const paths = process.argv.slice(2).map(path => resolve(path));
assert.ok(paths.length === 1 || paths.length === 2, 'supply one or two vc.mjs builds');
const warmups = Number(process.env.WEB_STARTUP_WARMUPS || 2);
const samples = Number(process.env.WEB_STARTUP_SAMPLES || 9);
assert.ok(Number.isInteger(warmups) && warmups >= 1);
assert.ok(Number.isInteger(samples) && samples >= 3);
const records = paths.map(() => ({ warmup: [], samples: [] }));

async function sample(path) {
  // libuv's piped stdio uses socketpair, which restricted Linux sandboxes
  // can reject. Regular private files need no sockets and cannot back up.
  const directory = await mkdtemp(join(tmpdir(), 'vc-startup-'));
  const stdoutPath = join(directory, 'stdout');
  const stderrPath = join(directory, 'stderr');
  const stdoutFile = await open(stdoutPath, 'w');
  const stderrFile = await open(stderrPath, 'w');
  try {
    const code = await new Promise((done, reject) => {
      const child = spawn(process.execPath, [script, '--sample', path], {
        stdio: ['ignore', stdoutFile.fd, stderrFile.fd],
      });
      const timer = setTimeout(() => {
        child.kill('SIGKILL');
        reject(new Error(`startup exceeded ${timeout} ms: ${path}`));
      }, timeout);
      child.on('error', error => { clearTimeout(timer); reject(error); });
      child.on('close', result => { clearTimeout(timer); done(result); });
    });
    const stdout = await readFile(stdoutPath, 'utf8');
    const stderr = await readFile(stderrPath, 'utf8');
    assert.equal(code, 0, `startup exited ${code}: ${path}\n${stderr}`);
    try { return JSON.parse(stdout); }
    catch (error) { throw new Error(`invalid startup record: ${stdout}\n${stderr}`, { cause: error }); }
  } finally {
    await stdoutFile.close();
    await stderrFile.close();
    await unlink(stdoutPath);
    await unlink(stderrPath);
    await rmdir(directory);
  }
}

function median(values) {
  const ordered = [...values].sort((a, b) => a - b);
  const middle = Math.floor(ordered.length / 2);
  return ordered.length % 2 ? ordered[middle] : (ordered[middle - 1] + ordered[middle]) / 2;
}

for (let round = 0; round < warmups + samples; ++round) {
  const order = paths.map((_, index) => index);
  if (round % 2) order.reverse();
  for (const index of order) {
    const result = await sample(paths[index]);
    records[index][round < warmups ? 'warmup' : 'samples'].push(result);
    console.error(`${round < warmups ? 'warmup' : 'sample'} ${round + 1} ${paths[index]}: ` +
      `${result.first_screen_ms.toFixed(3)} ms (${result.runtime_ms.toFixed(3)} ms runtime)`);
  }
}

const results = records.map((record, index) => {
  const all = [...record.warmup, ...record.samples];
  assert.equal(new Set(all.map(row => row.wasm_sha256)).size, 1, 'build must not change during sampling');
  assert.ok(all.every(row => row.node === process.version && row.lazy_fetches === 0));
  return {
    path: paths[index], node: process.version,
    wasm_sha256: all[0].wasm_sha256, wasm_bytes: all[0].wasm_bytes,
    warmup_ms: record.warmup.map(row => row.first_screen_ms),
    samples_ms: record.samples.map(row => row.first_screen_ms),
    runtime_samples_ms: record.samples.map(row => row.runtime_ms),
    median_ms: median(record.samples.map(row => row.first_screen_ms)),
    runtime_median_ms: median(record.samples.map(row => row.runtime_ms)),
  };
});
const report = { timer: 'fresh Node import through first complete VC screen; no lazy fetches', results };
if (results.length === 2) {
  report.delta_ms = results[1].median_ms - results[0].median_ms;
  report.runtime_delta_ms = results[1].runtime_median_ms - results[0].runtime_median_ms;
}
console.log(JSON.stringify(report));
