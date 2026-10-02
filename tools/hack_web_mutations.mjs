// Briefs 33/36: prove each Hack browser gate using isolated publication copies.
// Only the named private replacement is changed; live build bytes stay put.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { closeSync, openSync } from 'node:fs';
import { mkdir, mkdtemp, readFile, readdir, rm, symlink, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';

const root = resolve(import.meta.dirname, '..');
const publication = resolve(process.argv[2] || 'build/web');
const names = await readdir(publication);
const logs = join(root, 'build/hack-web-mutations');
await mkdir(logs, { recursive: true });
const named = pattern => {
  const matches = names.filter(name => pattern.test(name));
  assert.equal(matches.length, 1, `one published ${pattern}`);
  return matches[0];
};
const hack = named(/^hack\.[0-9a-f]{12}\.wasm$/);
const rogue = named(/^rogue\.[0-9a-f]{12}\.wasm$/);
const hackBytes = await readFile(join(root, 'build/hack/HACK.EXE'));
const hackHash = createHash('sha256').update(hackBytes).digest('hex');
const hackFile = 'file.' + hackHash.slice(0, 12) + '.bin';
assert.ok(names.includes(hackFile), 'Hack uses the shared per-file first-OPEN publication');
const main = await readFile(join(publication, 'vc.wasm'));
const runtime = await readFile(join(publication, 'vc.mjs'), 'utf8');
const html = await readFile(join(publication, 'index.html'), 'utf8');

function replaceMain(from, to) {
  assert.equal(Buffer.byteLength(from), Buffer.byteLength(to), 'binary mutations preserve section sizes');
  const bytes = Buffer.from(main);
  const offset = bytes.indexOf(from);
  assert.ok(offset >= 0 && bytes.indexOf(from, offset + 1) < 0, 'exactly one main string ' + JSON.stringify(from));
  bytes.set(Buffer.from(to), offset);
  return bytes;
}

function replaceRuntime(from, to) {
  assert.equal(runtime.split(from).length, 2, 'exactly one generated runtime anchor ' + from);
  return runtime.replace(from, to);
}

const rename = 'rename(old_node,new_dir,new_name){';
const rmdir = 'rmdir(parent,name){var node=FS.lookupNode(parent,name);';
const corruptFile = Buffer.from(hackBytes);
corruptFile[corruptFile.length - 1] ^= 1;
const wrongHash = (hackHash[0] === '0' ? '1' : '0') + hackHash.slice(1);
assert.ok(html.includes('</head>'), 'the first-load mutation needs the actual built page head');
const mutations = [
  { name: 'hack-side-image', gate: 'web_modules.mjs', expected: /hack exports only its own image/,
    replacements: new Map([[hack, await readFile(join(publication, rogue))]]) },
  { name: 'hack-file-integrity', gate: 'web_assets.mjs', expected: /lazy-file hash differs from its actual bytes/,
    replacements: new Map([[hackFile, corruptFile]]) },
  { name: 'eager-hack-file', gate: 'web_size.mjs', expected: /Hack translations and lazy DOS files must be absent/,
    replacements: new Map([['index.html', html.replace('</head>',
      '<link rel="preload" as="fetch" href="./' + hackFile + '">\n</head>')]]) },
  { name: 'hack-runtime-registry', gate: 'web_smoke.mjs', args: ['--hack-only'],
    expected: /browser translation hack\.[0-9a-f]+\.wasm lacks its image\/runner/,
    replacements: new Map([['vc.wasm', replaceMain('image_hack\0', 'image_FAKE\0')]]) },
  { name: 'source-directory-marker', gate: 'web_smoke.mjs', args: ['--source-only', '--no-webcrypto'],
    expected: /renaming a parent retains the unopened inode marker/,
    replacements: new Map([['vc.mjs', replaceRuntime(rename,
      rename + 'if(old_node.name==="SRC")delete old_node.contents["VCOVL.ASM"].vcLazyFile;')]]) },
  { name: 'unopened-parent-directory-move', gate: 'web_smoke.mjs', args: ['--lazy-parent-move', '--no-webcrypto'],
    expected: /moving an unopened ancestor preserves the executable lazy marker/,
    replacements: new Map([['vc.mjs', replaceRuntime(rename,
      rename + 'if(old_node.name==="GAMES")delete old_node.contents.HACK.contents["HACK.EXE"].vcLazyFile;')]]) },
  { name: 'unopened-directory-rmdir', gate: 'web_smoke.mjs', args: ['--lazy-rmdir', '--no-webcrypto'],
    expected: /waiting for DOS refuses removal of the nonempty lazy directory/,
    replacements: new Map([['vc.mjs', replaceRuntime(rmdir,
      rmdir + 'if(name==="HACK")node.contents={};')]]) },
  { name: 'downloaded-file-sha256', gate: 'web_smoke.mjs', args: ['--hack-only', '--no-webcrypto'],
    expected: /waiting for hack\.wasm first-use download request/,
    replacements: new Map([['vc.wasm', replaceMain(hackHash + '\0', wrongHash + '\0')]]) },
];

function runGate(gate, site, args, label) {
  const path = join(logs, `${label}.log`);
  const descriptor = openSync(path, 'w');
  let result;
  try {
    result = spawnSync(process.execPath,
      [join(root, 'tests', gate), gate === 'web_smoke.mjs' ? join(site, 'vc.mjs') : site, ...(args || [])],
      { cwd: root, stdio: ['ignore', descriptor, descriptor], timeout: 90000,
        env: { ...process.env, WEB_SMOKE_TIMEOUT: '5000' } });
  } finally { closeSync(descriptor); }
  assert.equal(result.error, undefined, `${label}: child failed to run: ${result.error}`);
  return { status: result.status, path };
}

const temporary = await mkdtemp(join(tmpdir(), 'vc-hack-web-mutations-'));
try {
  const originalHash = await readFile(join(root, 'runtime/web_sha256.c'), 'utf8');
  const brokenHash = originalHash.replace('0x6a09e667', '0x6a09e666');
  assert.notEqual(brokenHash, originalHash, 'SHA-256 initial-state mutation applies');
  const implementation = join(temporary, 'sha256.c');
  const executable = join(temporary, 'sha256-test');
  await writeFile(implementation, brokenHash);
  const compile = spawnSync('cc', ['-O1', '-std=c11', '-I', join(root, 'runtime'),
    join(root, 'tests/test_web_sha256.c'), implementation, '-o', executable],
    { cwd: root, encoding: 'utf8' });
  assert.equal(compile.status, 0, compile.stdout + compile.stderr);
  const hashLog = join(logs, '0-sha256-initial-state.log');
  const descriptor = openSync(hashLog, 'w');
  const hashRun = spawnSync(executable, [], { cwd: temporary, stdio: ['ignore', descriptor, descriptor] });
  closeSync(descriptor);
  assert.ok(hashRun.status !== 0 || hashRun.signal, 'broken SHA-256 must fail a standard vector');
  assert.match(await readFile(hashLog, 'utf8'), /SHA-256 vector failed/);
  console.log('RED 0-sha256-initial-state: one-bit initial-state defect fails the standard empty-message vector');
  for (let index = 0; index < mutations.length; index++) {
    const mutation = mutations[index];
    const site = join(temporary, `site-${index}`);
    await mkdir(site);
    for (const name of names) {
      if (mutation.replacements.has(name)) await writeFile(join(site, name), mutation.replacements.get(name));
      else await symlink(join(publication, name), join(site, name));
    }
    const label = `${index + 1}-${mutation.name}`;
    const result = runGate(mutation.gate, site, mutation.args, label);
    const output = await readFile(result.path, 'utf8');
    assert.notEqual(result.status, 0, `${label}: planted defect unexpectedly passed`);
    assert.match(output, mutation.expected, `${label}: failed for an unrelated reason; see ${result.path}`);
    const diagnostic = output.split('\n').find(line => mutation.expected.test(line));
    console.log(`RED ${label}: ${diagnostic.trim()}`);
  }
  for (const gate of ['web_assets.mjs', 'web_modules.mjs', 'web_size.mjs', 'web_smoke.mjs']) {
    const result = runGate(gate, publication, [], `restored-${gate}`);
    assert.equal(result.status, 0, await readFile(result.path, 'utf8'));
    console.log(`GREEN restored ${gate}`);
  }
  for (const mode of ['--hack-only', '--source-only', '--lazy-parent-move', '--lazy-rmdir']) {
    const result = runGate('web_smoke.mjs', publication, [mode, '--no-webcrypto'], `restored-${mode.slice(2)}-http`);
    assert.equal(result.status, 0, await readFile(result.path, 'utf8'));
    console.log(`GREEN restored ${mode}, with WebCrypto unavailable`);
  }
  const restoredHash = spawnSync(join(root, 'build/test_web_sha256'), [], { encoding: 'utf8' });
  assert.equal(restoredHash.status, 0, restoredHash.stdout + restoredHash.stderr);
  console.log(`GREEN restored standard SHA-256 vectors`);
  console.log(`Hack web mutations: ${mutations.length + 1} isolated defects rejected; live files unchanged; logs in ${logs}`);
} finally {
  // This exact private directory was created above, never an inferred root.
  await rm(temporary, { recursive: true, force: true });
}
