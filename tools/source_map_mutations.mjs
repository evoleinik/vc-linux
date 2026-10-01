// Demonstrate the source-map gates on isolated publication/repository copies.
// No checked-out implementation or live build output is modified. Rehash each
// mutated map/index so coverage and provenance defects reach their own oracle,
// rather than merely tripping the immutable-name check first.
import assert from 'node:assert/strict';
import { readFile, writeFile, mkdir, mkdtemp, symlink, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

const root = resolve(import.meta.dirname, '..');
const output = resolve(process.argv[2] || 'build/web');
const indexName = (await readFile(`${output}-work/source-index-name.txt`, 'utf8')).trim();
const originalIndex = JSON.parse(await readFile(join(output, indexName)));
const originalMaps = {};
const originalSources = new Map();
for (const [name, item] of Object.entries(originalIndex.images)) {
  const map = JSON.parse(await readFile(join(output, item.url)));
  originalMaps[name] = map;
  for (const file of map.files || []) {
    originalSources.set(file.url, await readFile(join(output, file.url)));
  }
}
const builder = await readFile(join(root, 'tools/source_maps.py'), 'utf8');
const gate = await readFile(join(root, 'tests/test_source_maps.mjs'), 'utf8');
const hash = data => createHash('sha256').update(data).digest('hex').slice(0, 12);
const serialize = value => Buffer.from(`${JSON.stringify(value)}\n`);
const seedOffset = { 'VC.COM': 10, 'VC.OVL': 12 };

function firstOrdinary(map) {
  // Seed addresses come from the independent listing oracle's first sample.
  // VCOVL's Start begins with CLD; VC.COM's RESIDENT bytes precede its JMP.
  const record = map.lines.find(record => record[0] === seedOffset[map.image]);
  assert.ok(record, `${map.image}: mutation seed no longer exists`);
  return record;
}

const mutations = [
  ...['VC.COM', 'VC.OVL'].map(image => ({
    name: `${image}: missing instruction`, expected: /listed instruction has no source mapping/,
    mutate: ({ maps }) => { const row = firstOrdinary(maps[image]); maps[image].lines = maps[image].lines.filter(item => item !== row); },
  })),
  { name: 'VC.COM: duplicate instruction', expected: /duplicate\/unsorted instruction mapping/,
    mutate: ({ maps }) => maps['VC.COM'].lines.splice(1, 0, [...maps['VC.COM'].lines[0]]) },
  ...['VC.COM', 'VC.OVL'].map(image => ({
    name: `${image}: wrong sampled line`, expected: /sample lost its original mnemonic/,
    mutate: ({ maps }) => { firstOrdinary(maps[image])[2] = 1; },
  })),
  { name: 'CALL return-address metadata', expected: /return metadata must contain exactly listing CALL ends/,
    mutate: ({ maps }) => maps['VC.OVL'].calls.shift() },
  { name: 'Original comments/text', expected: /original text, comments and line endings/,
    mutate: ({ maps, sources }) => {
      const file = maps['VC.COM'].files.find(file => file.name === 'VC.ASM');
      const bytes = Buffer.from(sources.get(file.url)); bytes[0] = 0x21;
      const url = `source-vc.${hash(bytes)}.txt`;
      sources.set(url, bytes);
      for (const map of Object.values(maps)) for (const item of map.files || []) if (item.url === file.url) item.url = url;
    } },
  { name: 'Truthful include breadcrumb', expected: /include breadcrumb must name an actual parent INCLUDE line/,
    mutate: ({ maps }) => { maps['VC.OVL'].files.find(file => file.name === 'VCKEYB.INC').via[0].line++; } },
  { name: 'Rogue cannot invent C lines', expected: /compiled C must not claim invented C line numbers/,
    mutate: ({ maps }) => { maps['ROGUE.EXE'].lines = [[0, 0, 1, 1]]; } },
  { name: 'Content-addressed source names', expected: /source asset hash disagrees with bytes/,
    mutate: ({ maps, sources }) => {
      const file = maps['VC.COM'].files.find(file => file.name === 'VC.ASM');
      const bytes = Buffer.from(sources.get(file.url)); bytes[0] = 0x21;
      sources.set(file.url, bytes); // deliberately retain the now-invalid hash
    } },
  { name: 'CP866 decoding', expected: /CP866 fallback did not preserve the original comment/,
    mutate: state => {
      assert.ok(state.builder.includes('data.decode(legacy_encoding)'));
      state.builder = state.builder.replace('data.decode(legacy_encoding)', 'data.decode("latin-1")');
    } },
];

const temporary = await mkdtemp(join(tmpdir(), 'vc-source-mutations-'));
try {
  const repo = join(temporary, 'repo');
  await mkdir(join(repo, 'tests'), { recursive: true });
  await mkdir(join(repo, 'tools'), { recursive: true });
  await writeFile(join(repo, 'tests/test_source_maps.mjs'), gate);
  for (const name of ['asm', 'third_party', 'translator', '.venv', 'build']) {
    await symlink(join(root, name), join(repo, name));
  }
  for (const name of ['build_gwbasic.py', 'build_vz.py', 'web_modules.py', 'jwasm']) {
    await symlink(join(root, 'tools', name), join(repo, 'tools', name));
  }
  for (let sequence = 0; sequence < mutations.length; sequence++) {
    const mutation = mutations[sequence];
    const state = { maps: structuredClone(originalMaps), sources: new Map(originalSources), builder };
    mutation.mutate(state);
    await writeFile(join(repo, 'tools/source_maps.py'), state.builder);
    // Python bytecode cache timestamps have one-second granularity. A changed
    // implementation must not accidentally reuse the preceding mutant's pyc.
    const site = join(temporary, `site-${sequence}`);
    await mkdir(site);
    await mkdir(`${site}-work`);
    const index = structuredClone(originalIndex);
    for (const [image, map] of Object.entries(state.maps)) {
      const data = serialize(map);
      const url = `source-map-${image.toLowerCase().replace('.', '-')}.${hash(data)}.json`;
      await writeFile(join(site, url), data);
      index.images[image].url = url;
    }
    for (const [name, bytes] of state.sources) await writeFile(join(site, name), bytes);
    const indexData = serialize(index);
    const name = `source-index.${hash(indexData)}.json`;
    await writeFile(join(site, name), indexData);
    await writeFile(`${site}-work/source-index-name.txt`, `${name}\n`);
    const run = spawnSync(process.execPath, [join(repo, 'tests/test_source_maps.mjs'), site], {
      cwd: repo, encoding: 'utf8', maxBuffer: 4 * 1024 * 1024,
      env: { ...process.env, PYTHONDONTWRITEBYTECODE: '1', PYTHONPYCACHEPREFIX: join(temporary, `pycache-${sequence}`) },
    });
    assert.notEqual(run.status, 0, `${mutation.name}: planted defect unexpectedly passed`);
    assert.match(run.stdout + run.stderr, mutation.expected, `${mutation.name}: failed for an unrelated reason`);
    const diagnostic = (run.stdout + run.stderr).split('\n').find(line => mutation.expected.test(line));
    console.log(`RED ${mutation.name}: ${diagnostic.trim()}`);
  }
  const restored = spawnSync(process.execPath, [join(root, 'tests/test_source_maps.mjs'), output], {
    cwd: root, encoding: 'utf8', maxBuffer: 4 * 1024 * 1024,
  });
  assert.equal(restored.status, 0, restored.stdout + restored.stderr);
  console.log(`GREEN restored originals: ${mutations.length} isolated defects rejected; live implementation and output were never modified.`);
  process.stdout.write(restored.stdout);
} finally {
  await rm(temporary, { recursive: true, force: true });
}
