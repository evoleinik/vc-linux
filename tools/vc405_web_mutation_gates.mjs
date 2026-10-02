// Brief 32: exercise the real static gates on isolated publication copies.
// node tools/vc405_web_mutation_gates.mjs [build/web] [build/vc405-web-mutations]
// No browser, server, network, checked-out source, or live build is modified.
// Rehash source maps/indexes before testing their semantic defects, so those
// failures cannot be mistaken for the separate immutable-filename gate.
import assert from 'node:assert/strict';
import { copyFile, cp, mkdir, mkdtemp, open, readFile, readdir, rm, symlink, unlink, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { basename, join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const output = resolve(process.argv[2] || join(root, 'build/web'));
const evidence = resolve(process.argv[3] || join(root, 'build/vc405-web-mutations'));
const temporary = await mkdtemp(join(tmpdir(), 'vc405-web-mutations-'));
const snapshot = join(temporary, 'original');
const repo = join(temporary, 'repo');
const hash = bytes => createHash('sha256').update(bytes).digest('hex').slice(0, 12);
const serialize = value => Buffer.from(`${JSON.stringify(value)}\n`);
const summary = [];

function report(text) {
  summary.push(text);
  console.log(text);
}

// A site initially contains symlinks to our private snapshot, never hardlinks.
// Unlink the private directory entry before writing, so neither the snapshot
// nor an original file can be altered by following a symlink accidentally.
async function owned(directory, name, bytes) {
  assert.equal(basename(name), name, `unsafe mutation filename: ${name}`);
  const path = join(directory, name);
  try { await unlink(path); } catch (error) { if (error.code !== 'ENOENT') throw error; }
  await writeFile(path, bytes, { flag: 'wx' });
}

async function run(name, executable, args, cwd = repo) {
  // Node's pipe capture can report EPERM in this managed sandbox. Ordinary
  // private output files preserve the real child diagnostics without a pipe.
  const stdoutPath = join(temporary, `${name}-stdout`);
  const stderrPath = join(temporary, `${name}-stderr`);
  const stdout = await open(stdoutPath, 'wx');
  const stderr = await open(stderrPath, 'wx');
  let result;
  try {
    result = spawnSync(executable, args, {
      cwd, timeout: 60_000, stdio: ['ignore', stdout.fd, stderr.fd],
      env: { ...process.env, BUILD_DIR: join(root, 'build'), PYTHONDONTWRITEBYTECODE: '1',
        PYTHONPYCACHEPREFIX: join(temporary, 'pycache') },
    });
  } finally {
    await stdout.close();
    await stderr.close();
  }
  result.stdout = await readFile(stdoutPath, 'utf8');
  result.stderr = await readFile(stderrPath, 'utf8');
  await writeFile(join(evidence, `${name}.log`),
    `$ ${executable} ${args.join(' ')}\nstatus: ${result.status}; signal: ${result.signal}; spawn diagnostic: ${result.error || 'none'}\n` +
    `stdout:\n${result.stdout || ''}\nstderr:\n${result.stderr || ''}\n`);
  assert.ifError(result.error);
  assert.equal(result.signal, null, `${name}: gate was killed instead of asserting`);
  return result;
}

function green(name, result) {
  assert.equal(result.status, 0, `${name}\n${result.stdout}\n${result.stderr}`);
}

function red(name, result, expected) {
  assert.equal(result.status, 1, `${name}: planted defect did not fail normally\n${result.stdout}\n${result.stderr}`);
  const diagnostic = `${result.stdout}\n${result.stderr}`.split('\n').find(line => expected.test(line));
  assert.ok(diagnostic, `${name}: failed for an unrelated reason\n${result.stdout}\n${result.stderr}`);
  report(`RED ${name}: ${diagnostic.trim()}`);
}

function firstLoad(result) {
  const match = result.stdout.match(/web first load: ([\d,]+) gzip bytes/);
  assert.ok(match, 'size gate no longer prints its first-load measurement');
  return Number(match[1].replaceAll(',', ''));
}

try {
  await mkdir(evidence, { recursive: true });
  // Snapshot once: a concurrent production rebuild cannot change a mutation's
  // side files halfway through its oracle. Validate this snapshot first.
  await cp(output, snapshot, { recursive: true, dereference: true });
  await mkdir(`${snapshot}-work`);
  await copyFile(`${output}-work/source-index-name.txt`, `${snapshot}-work/source-index-name.txt`);
  await mkdir(join(repo, 'tests'), { recursive: true });
  for (const name of ['web_size.mjs', 'web_assets.mjs', 'test_source_maps.mjs']) {
    await copyFile(join(root, 'tests', name), join(repo, 'tests', name));
  }
  // Source-map tests only read these dependencies; their generated CP866
  // fixture and any Python cache stay under temporary, not in the checkout.
  for (const name of ['asm', 'asm405', 'third_party', 'tools', 'translator', '.venv', 'build']) {
    await symlink(join(root, name), join(repo, name));
  }
  const gate = (name, script, site) => run(name, process.execPath, [join(repo, 'tests', script), site]);
  const size = await gate('baseline-size', 'web_size.mjs', snapshot);
  green('baseline size', size);
  green('baseline assets', await gate('baseline-assets', 'web_assets.mjs', snapshot));
  green('baseline source maps', await gate('baseline-source', 'test_source_maps.mjs', snapshot));
  const before = firstLoad(size);
  report(`GREEN baseline: ${before.toLocaleString('en')} gzip bytes; assets and all eight Source registrations pass.`);

  const indexName = (await readFile(`${snapshot}-work/source-index-name.txt`, 'utf8')).trim();
  const originalIndex = JSON.parse(await readFile(join(snapshot, indexName)));
  const maps = Object.fromEntries(await Promise.all(Object.entries(originalIndex.images).map(async ([image, item]) =>
    [image, JSON.parse(await readFile(join(snapshot, item.url)))])));
  const entries = await readdir(snapshot);

  async function site(name) {
    const path = join(temporary, name);
    await mkdir(path);
    await mkdir(`${path}-work`);
    for (const entry of entries) await symlink(join(snapshot, entry), join(path, entry));
    await writeFile(`${path}-work/source-index-name.txt`, `${indexName}\n`);
    return path;
  }

  async function publishIndex(path, index) {
    const bytes = serialize(index);
    const name = `source-index.${hash(bytes)}.json`;
    await owned(path, name, bytes);
    await writeFile(`${path}-work/source-index-name.txt`, `${name}\n`);
    const javascript = await readFile(join(snapshot, 'vc-web.js'), 'utf8');
    assert.ok(javascript.includes(indexName), 'published Source index is not wired into vc-web.js');
    await owned(path, 'vc-web.js', javascript.replaceAll(indexName, name));
  }

  async function publishMap(path, index, image, map) {
    const bytes = serialize(map);
    const name = `source-map-${image.toLowerCase().replace('.', '-')}.${hash(bytes)}.json`;
    await owned(path, name, bytes);
    index.images[image].url = name;
    await publishIndex(path, index);
  }

  async function mutation(name, script, modify, expected, check = () => {}) {
    const path = await site(name);
    await modify(path);
    const result = await gate(`${name}-red`, script, path);
    red(name, result, expected);
    check(result);
    green(`${name} restored`, await gate(`${name}-restored`, script, snapshot));
    report(`GREEN ${name} restored: the unchanged snapshot passes the same gate.`);
  }

  await mutation('first-load-reduction', 'web_size.mjs', async path => {
    // Deterministic incompressible padding in valid, eagerly imported JS.
    // Target roughly 1.2 MB: below the old absolute cap, above the new cap
    // implied by a 150,000-byte reduction. The independent gate counts it.
    const padding = Buffer.alloc(1_200_000 - before);
    for (let offset = 0; offset < padding.length; offset += 32) {
      const block = createHash('sha256').update(`Brief 32 eager padding ${offset}`).digest();
      block.copy(padding, offset, 0, Math.min(block.length, padding.length - offset));
    }
    await owned(path, 'mutation-eager.js', `export default "${padding.toString('base64')}";\n`);
    const page = await readFile(join(snapshot, 'index.html'), 'utf8');
    const version = page.match(/\?v=([a-f0-9]{12})/)?.[1];
    assert.ok(version && page.includes('</head>'));
    await owned(path, 'index.html', page.replace('</head>',
      `<script type="module" src="./mutation-eager.js?v=${version}"></script>\n</head>`));
  }, /lazy H: files saved \d+ bytes; need at least 150000/, result => {
    const bytes = firstLoad(result);
    assert.ok(bytes <= 1_300_000 && bytes > 1_149_288,
      `mutation must isolate the reduction gate, not the absolute 1.3 MB cap: ${bytes}`);
    report(`  Reduction-only witness: ${bytes.toLocaleString('en')} gzip bytes remains below 1,300,000.`);
  });

  await mutation('lazy-file-content-hash', 'web_assets.mjs', async path => {
    const name = entries.filter(name => /^file\.[0-9a-f]{12}\.bin$/.test(name)).sort()[0];
    assert.ok(name, 'no immutable lazy file to mutate');
    const bytes = await readFile(join(snapshot, name));
    assert.ok(bytes.length);
    bytes[0] ^= 1;
    await owned(path, name, bytes); // retain the old, now-invalid content hash
  }, /lazy-file hash differs from its actual bytes/);

  for (const image of ['VC405.COM', 'VCSETUP.COM']) {
    const stem = image === 'VC405.COM' ? 'vc405' : 'vcsetup405';
    await mutation(`${stem}-source-registration`, 'test_source_maps.mjs', async path => {
      const index = structuredClone(originalIndex);
      delete index.images[image];
      await publishIndex(path, index);
    }, /Expected values to be strictly deep-equal/, result => {
      assert.ok(result.stderr.includes(`'${image}'`), `missing ${image} was not identified by the registration oracle`);
    });

    await mutation(`${stem}-source-tree`, 'test_source_maps.mjs', async path => {
      const map = structuredClone(maps[image]);
      const wrongTree = maps['VC.COM'].files.find(file => file.path === 'asm/VC.ASM');
      assert.ok(wrongTree);
      // Preserve truthful text, encoding and breadcrumbs, but route one
      // real 4.05 instruction into the newer VC's original source tree.
      map.files.push(structuredClone(wrongTree));
      map.lines[0][1] = map.files.length - 1;
      map.lines[0][2] = 1;
      await publishMap(path, structuredClone(originalIndex), image, map);
    }, /every Source location belongs to the unedited 4\.05 source tree/);

    await mutation(`${stem}-source-instruction`, 'test_source_maps.mjs', async path => {
      const map = structuredClone(maps[image]);
      const offset = map.calls[0][1];
      assert.ok(map.lines.some(row => row[0] === offset));
      map.lines = map.lines.filter(row => row[0] !== offset);
      await publishMap(path, structuredClone(originalIndex), image, map);
    }, /listed instruction has no source mapping/);

    await mutation(`${stem}-source-call`, 'test_source_maps.mjs', async path => {
      const map = structuredClone(maps[image]);
      assert.ok(map.calls.length);
      map.calls.shift();
      await publishMap(path, structuredClone(originalIndex), image, map);
    }, /return metadata must contain exactly listing CALL ends/);
  }

  // Copy the real regression test to a private root, so its __file__-based
  // build/gen resolution cannot accidentally select the live generated C.
  const regression = join(temporary, 'regression');
  const generated = join(regression, 'build/gen');
  await mkdir(join(regression, 'tests'), { recursive: true });
  await mkdir(generated, { recursive: true });
  const test = await readFile(join(root, 'tests/test_translator_regression.py'), 'utf8');
  await writeFile(join(regression, 'tests/test_translator_regression.py'), test);
  const earlier = [...test.matchAll(/^\s+"([a-z_]+\.c)": "[0-9a-f]{64}",$/gm)].map(match => match[1]);
  assert.equal(earlier.length, 15, 'regression mutation must cover the full earlier-program baseline');
  for (const name of earlier) await symlink(join(root, 'build/gen', name), join(generated, name));
  const regressionGate = name => run(name, join(root, '.venv/bin/python'),
    ['-m', 'pytest', '-q', '-p', 'no:cacheprovider', '--noconftest', 'tests/test_translator_regression.py'], regression);
  green('prior C baseline', await regressionGate('prior-c-baseline'));
  const original = await readFile(join(root, 'build/gen/command.c'));
  const changed = Buffer.from(original);
  changed[0] ^= 1;
  await owned(generated, 'command.c', changed);
  red('prior-generated-c', await regressionGate('prior-c-red'), /command\.c changed while adding VC 4\.05/);
  await owned(generated, 'command.c', original);
  green('prior C restored', await regressionGate('prior-c-restored'));
  report('GREEN prior-generated-c restored: all 15 earlier generated-C hash comparisons pass.');
  report('GREEN complete: 11 isolated defects rejected and individually restored; live source and build output were never modified.');
} finally {
  await writeFile(join(evidence, 'summary.log'), `${summary.join('\n')}\n`).catch(() => {});
  // temporary is exactly the path returned by mkdtemp above, never an input.
  await rm(temporary, { recursive: true, force: true });
}
