// No browser or sockets: actual MEMFS, C cache and shared Asyncify transport.
// The full VC smoke separately gates F3, DOS errors and dispatcher suspension.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const modulePath = resolve(process.argv[2] || 'build/web-work/files-test.mjs');
const { default: createFixture } = await import(pathToFileURL(modulePath));
const bytes = (index) => index === 10 ? new Uint8Array() :
  new TextEncoder().encode(`${String.fromCharCode(65 + (index === 12 ? 0 : index))}bytes`);
const requests = new Map();
const replies = new Map();
let fixture;
fixture = await createFixture({
  wasmBinary: readFileSync(modulePath.replace(/\.mjs$/, '.wasm')),
  locateFile: (name) => `${name}?v=page-version`,
  vcProgramFetchTimeoutMs: 1000,
  vcFetchProgram: async (url, options) => {
    const match = url.match(/^file\.(\d{12})\.bin\?v=page-version$/);
    assert.ok(match, `same immutable URL + locateFile convention as modules: ${url}`);
    const index = Number(match[1]);
    requests.set(index, (requests.get(index) || 0) + 1);
    // At least one event-loop turn proves that these are real Asyncify waits.
    await new Promise((done) => setTimeout(done, 0));
    return replies.has(index) ? replies.get(index)(options) : bytes(index);
  },
});
fixture.ccall('fixture_init', null, [], []);
const install = (index, path) => assert.equal(
  fixture.ccall('fixture_install', 'number', ['number', 'string'], [index, path]), 0);
const open = (path, action = 0) => fixture.ccall(
  'fixture_open', 'number', ['string', 'number'], [path, action], { async: true });
const reference = (index) => fixture.ccall(
  'fixture_reference', 'number', ['number'], [index], { async: true });
const read = (path) => fixture.FS.readFile(path);
const count = (index) => requests.get(index) || 0;
const check = (path, index) => assert.deepEqual(read(path), bytes(index));
const tick = () => new Promise((done) => setTimeout(done, 0));
async function bounded(promise, description) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`unbounded ${description}`)), 500);
    })]);
  } finally { clearTimeout(timer); }
}
const adler32 = (contents) => {
  let a = 1, b = 0;
  for (const byte of contents) { a = (a + byte) % 65521; b = (b + a) % 65521; }
  return (b << 16 | a) >>> 0;
};

// Eager installation used the current MEMFS creation time, not the source
// archive's timestamp. Freeze only this synchronous installation so neither
// Asyncify waits nor transport deadlines observe a synthetic clock.
const originalTime = Date.UTC(2003, 6, 9, 10, 11, 12, 345);
const realDateNow = Date.now;
try {
  Date.now = () => originalTime;
  install(0, '/files/first.txt');
  fixture.FS.writeFile('/files/eager-clock.txt', bytes(0));
} finally {
  Date.now = realDateNow;
}
const initialMetadata = fixture.FS.stat('/files/first.txt');
const eagerMetadata = fixture.FS.stat('/files/eager-clock.txt');
assert.equal(initialMetadata.mtime.getTime(), originalTime, 'real creation date before first open');
assert.equal(initialMetadata.mtime.getTime(), eagerMetadata.mtime.getTime(), 'same initial date as eager installation');
assert.equal(initialMetadata.ctime.getTime(), originalTime, 'real inode creation time before first open');
assert.equal(initialMetadata.size, 6, 'real size before first open');
assert.equal(requests.size, 0, 'metadata installation must not download content');
assert.throws(() => read('/files/first.txt'), 'an unopened placeholder must not leak zeros');
assert.equal(await open('/files/first.txt'), 0);
check('/files/first.txt', 0);
assert.equal(fixture.FS.stat('/files/first.txt').mtime.getTime(), originalTime, 'fetch preserves listing date');
assert.equal(await open('/files/first.txt'), 0);
assert.equal(count(0), 1, 'two successful DOS opens fetch once');
const edit = fixture.FS.open('/files/first.txt', 'r+');
fixture.FS.write(edit, new TextEncoder().encode('edited'), 0, 6, 0);
fixture.FS.close(edit);
assert.equal(await reference(0), 0, 'the complete private reference is not the edited DOS file');
assert.equal(await open('/files/first.txt'), 0);
assert.equal(new TextDecoder().decode(read('/files/first.txt')), 'edited');
assert.equal(count(0), 1);
install(12, '/files/identical.txt');
assert.equal(await open('/files/identical.txt'), 0);
check('/files/identical.txt', 12);
assert.equal(count(0), 1, 'identical immutable references share their successful download');

install(1, '/files/old.txt');
fixture.FS.rename('/files/old.txt', '/files/renamed.txt');
assert.equal(await open('/files/renamed.txt'), 0);
check('/files/renamed.txt', 1);
assert.equal(count(1), 1);
install(2, '/files/old-dir/nested.txt');
fixture.FS.rename('/files/old-dir', '/files/new-dir');
assert.equal(await open('/files/new-dir/nested.txt'), 0);
check('/files/new-dir/nested.txt', 2);
install(3, '/files/deleted.txt');
fixture.FS.unlink('/files/deleted.txt');
fixture.FS.writeFile('/files/deleted.txt', 'new user file');
assert.equal(await open('/files/deleted.txt'), 0);
assert.equal(count(3), 0, 'deleting/recreating does not resurrect old shipped content');
assert.equal(new TextDecoder().decode(read('/files/deleted.txt')), 'new user file');
install(4, '/files/truncated.txt');
assert.equal(await open('/files/truncated.txt', 3), 0);
assert.equal(read('/files/truncated.txt').length, 0);
assert.equal(await open('/files/truncated.txt'), 0);
assert.equal(count(4), 0, 'intentional truncation does not fetch the discarded contents');
install(13, '/files/existing.txt');
assert.equal(await open('/files/existing.txt', 4), 80);
assert.equal(count(13), 0, 'failed exclusive creation performs no fetch');
assert.equal(await open('/files/existing.txt'), 0);
check('/files/existing.txt', 13);
install(14, '/files/date.txt');
const changedTime = new Date('2000-06-01T12:00:00Z');
fixture.FS.utime('/files/date.txt', changedTime, changedTime);
assert.equal(await open('/files/date.txt'), 0);
check('/files/date.txt', 14);
assert.equal(fixture.FS.stat('/files/date.txt').mtime.getTime(), changedTime.getTime());

for (const index of [5, 6, 7, 15]) {
  const path = `/files/failure-${index}.txt`;
  install(index, path);
  const before = fixture.FS.stat(path);
  replies.set(index, () => {
    if (index === 5) throw new Error('offline');
    if (index === 6) return bytes(index).subarray(0, 3);
    if (index === 15) {
      const collision = bytes(index);
      collision[0]++; collision[1] -= 2; collision[2]++;
      assert.equal(adler32(collision), adler32(bytes(index)), 'the regression preserves the old weak checksum');
      assert.notDeepEqual(collision, bytes(index));
      return collision;
    }
    return Uint8Array.from(bytes(index), (byte) => byte ^ 1);
  });
  assert.equal(await open(path), 5, 'transport, size, checksum and same-Adler32 corruption are DOS errors');
  assert.equal(fixture.FS.stat(path).size, before.size);
  assert.equal(fixture.FS.stat(path).mtime.getTime(), before.mtime.getTime());
  assert.throws(() => read(path), 'failed fetch does not expose a zero-filled file');
  replies.delete(index);
  assert.equal(await open(path), 0);
  check(path, index);
  assert.equal(await open(path), 0);
  assert.equal(count(index), 2, 'only failed attempts retry; success is cached');
}

for (const index of [8, 9]) {
  const path = `/files/race-${index}.txt`;
  install(index, path);
  let release;
  let arrived;
  const waiting = new Promise((done) => { arrived = done; });
  replies.set(index, () => new Promise((done) => { release = done; arrived(); }));
  const result = open(path);
  await waiting;
  if (index === 8) fixture.FS.unlink(path);
  fixture.FS.writeFile(path, 'edited');
  release(bytes(index));
  assert.equal(await result, 5, 'a stale response cannot overwrite replacement or edited content');
  assert.equal(new TextDecoder().decode(read(path)), 'edited');
  assert.equal(await reference(index), 0, 'the validated private bytes may still be reused');
  assert.equal(await open(path), 0);
  assert.equal(count(index), 1);
}

install(10, '/files/empty.txt');
assert.equal(fixture.FS.stat('/files/empty.txt').size, 0);
assert.equal(await open('/files/empty.txt'), 0);
assert.equal(await open('/files/empty.txt'), 0);
assert.equal(read('/files/empty.txt').length, 0);
assert.equal(count(10), 1, 'empty success also has a non-null cached reference');

install(11, '/files/timeout.txt');
fixture.vcProgramFetchTimeoutMs = 25;
let releaseLate;
let aborted = false;
replies.set(11, ({ signal }) => new Promise((done) => {
  releaseLate = done;
  signal.addEventListener('abort', () => { aborted = true; });
}));
assert.equal(await open('/files/timeout.txt'), 5);
assert.ok(aborted, 'the same bounded transport aborts lazy file bodies');
assert.throws(() => read('/files/timeout.txt'));
releaseLate(bytes(11));
await tick();
assert.deepEqual(fixture.FS.readdir('/var/vc').sort(), ['.', '..'], 'a late response never stages bytes');
replies.delete(11);
assert.equal(await open('/files/timeout.txt'), 0);
check('/files/timeout.txt', 11);
assert.equal(count(11), 2);

// Content verification is itself asynchronous and belongs inside the same
// deadline. A late crypto completion must be no more dangerous than a late body.
install(4, '/files/digest-timeout.txt');
const subtle = globalThis.crypto.subtle;
const originalDigest = subtle.digest;
let releaseDigest;
subtle.digest = () => new Promise((done) => { releaseDigest = done; });
try {
  assert.equal(await bounded(open('/files/digest-timeout.txt'), 'digest wait'), 5,
    'a stalled digest cannot suspend DOS indefinitely');
  assert.equal(typeof releaseDigest, 'function', 'the complete body reached content verification');
  assert.throws(() => read('/files/digest-timeout.txt'));
} finally {
  subtle.digest = originalDigest;
}
releaseDigest(await originalDigest.call(subtle, 'SHA-256', bytes(4)));
await tick();
assert.deepEqual(fixture.FS.readdir('/var/vc').sort(), ['.', '..'], 'a late digest never stages bytes');
assert.equal(await open('/files/digest-timeout.txt'), 0);
check('/files/digest-timeout.txt', 4);
assert.equal(count(4), 2);

// A response double (or application transport) owns its original buffer.
// Mutating it during the async hash must not alter the verified staged bytes.
install(3, '/files/snapshot.txt');
const borrowed = bytes(3);
replies.set(3, () => borrowed);
subtle.digest = function (algorithm, data) {
  const result = originalDigest.call(this, algorithm, data);
  borrowed[0]++; borrowed[1] -= 2; borrowed[2]++;
  assert.equal(adler32(borrowed), adler32(bytes(3)));
  return result;
};
try {
  assert.equal(await open('/files/snapshot.txt'), 0);
  check('/files/snapshot.txt', 3);
  assert.equal(await reference(3), 0, 'the cache contains the verified snapshot, not the changed transport buffer');
} finally {
  subtle.digest = originalDigest;
}

// HTTP hosting without WebCrypto uses the same transport/cache and full
// SHA-256 pin, including Adler32 collisions, empty files and retry behavior.
const cryptoDescriptor = Object.getOwnPropertyDescriptor(globalThis, 'crypto');
try {
  Object.defineProperty(globalThis, 'crypto', { value: undefined, configurable: true });
  let corrupt = true;
  let downloads = 0;
  const plain = await createFixture({
    wasmBinary: readFileSync(modulePath.replace(/\.mjs$/, '.wasm')),
    vcFetchProgram: async (url) => {
      downloads++;
      const match = String(url).match(/file\.(\d{12})\.bin$/);
      assert.ok(match);
      const data = bytes(Number(match[1]));
      if (corrupt && data.length) {
        data[0]++; data[1] -= 2; data[2]++;
        assert.equal(adler32(data), adler32(bytes(Number(match[1]))));
      }
      await tick();
      return data;
    },
  });
  plain.ccall('fixture_init', null, [], []);
  assert.equal(plain.ccall('fixture_install', 'number', ['number', 'string'], [0, '/plain.txt']), 0);
  const plainOpen = () => plain.ccall('fixture_open', 'number', ['string', 'number'], ['/plain.txt', 0], { async: true });
  assert.equal(await plainOpen(), 5, 'without WebCrypto, full SHA-256 still rejects same-Adler32 corruption');
  assert.throws(() => plain.FS.readFile('/plain.txt'));
  corrupt = false;
  assert.equal(await plainOpen(), 0);
  assert.deepEqual(plain.FS.readFile('/plain.txt'), bytes(0));
  assert.equal(await plainOpen(), 0);
  assert.equal(downloads, 2, 'without WebCrypto, retry succeeds once and then stays cached');
  assert.equal(plain.ccall('fixture_install', 'number', ['number', 'string'], [10, '/empty.txt']), 0);
  assert.equal(await plain.ccall('fixture_open', 'number', ['string', 'number'], ['/empty.txt', 0], { async: true }), 0);
  assert.equal(plain.FS.readFile('/empty.txt').length, 0);
} finally {
  if (cryptoDescriptor) Object.defineProperty(globalThis, 'crypto', cryptoDescriptor);
  else delete globalThis.crypto;
}

console.log('web lazy files: complete metadata, once-only fetch, SHA-256/immutable EXEC references, rename/delete/write/truncate, corrupt/offline/stale/body+digest-timeout recovery, and no-WebCrypto integrity/cache passed');
