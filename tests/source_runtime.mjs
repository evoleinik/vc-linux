// Takes the compiled Emscripten fixture path, never the production program.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import test from "node:test";
import { pathToFileURL } from "node:url";

const source = readFileSync(new URL("../web/vc-source.js", import.meta.url), "utf8");
const { walkCallers } = await import(`data:text/javascript,${encodeURIComponent(source)}`);

const { default: create } = await import(pathToFileURL(resolve(
  process.argv[2] ?? "build/source-runtime.mjs")));
const snapshots = [];
let resolved;
try {
  await create({
    vcSourceSnapshot: value => snapshots.push(value),
    vcSourceResolved: value => { resolved = value; },
  });
} catch (error) {
  // Emscripten's minified module would otherwise fill the failure output.
  console.error(error.message);
  process.exit(1);
}
assert.equal(snapshots.length, 7);
const [snapshot, continuation, timer, casemap, direct, wrappedINT, wrappedCasemap] = snapshots;
assert.deepEqual(snapshot.raw, { cs: 0xf000, ip: 0x28, ss: 0x4000, sp: 0xfff0 });
assert.equal(snapshot.current.kind, "interrupt");
assert.equal(snapshot.current.image, "fixture");
assert.equal(snapshot.current.offset, 0x40);
assert.equal(snapshot.current.returnIP, 0x42);
assert.equal(snapshot.stackTruncated, false);
assert.equal(snapshot.recent.length, 1024);
assert.deepEqual(snapshot.recent.slice(0, 4).map(({ offset }) => offset), [500, 502, 501, 1199]);
assert.equal(snapshot.recent.at(-1).offset, 176);
assert.equal(new Set(snapshot.recent.map(({ offset }) => offset)).size, 1024);
assert.equal(continuation.current.kind, "continuation");
assert.equal(continuation.current.offset, 0x42);
assert.equal(direct.current.kind, "instruction");
assert.equal(direct.current.offset, 0x40);
assert.equal(direct.stack.length, 1024);
assert.equal(direct.stackTruncated, true);
assert.deepEqual(resolved, { image: "fixture", offset: 0x43, cs: 0x3000, ip: 0x43 });
console.log("source runtime: ring/idle retention, copied tails, INT context, read-only snapshot ABI passed");

// These boundaries match the literal CALL bytes in source_runtime.c. Run
// the real browser caller filter on the real C snapshot ABI, so FLAGS=0246h
// demonstrably produces a false caller unless the runtime skips its frame.
const fixture = { image: "fixture", kind: "asm", files: [{ name: "FIXTURE.ASM" }],
  lines: [[0x100, 0, 1, 5], [0x243, 0, 2, 3], [0x350, 0, 3, 3]],
  calls: [[0x105, 0x100, "far"], [0x246, 0x243, "near"], [0x353, 0x350, "near"]] };
const callers = value => walkCallers(value, name => name === fixture.image ? fixture : null,
  () => null).map(line => line.offset);
for (const [name, value] of [["INT", snapshot], ["IRQ continuation", continuation],
  ["timer IRET", timer]]) {
  await test(`${name} skips IP, CS and CALL-looking FLAGS`, () => {
    assert.deepEqual(callers(value), [0x350], "FLAGS=0246h must not become a near-CALL caller");
    assert.equal(value.stack.length, 5);
    assert.deepEqual(value.stack.map(word => word.sp), [0xfff6, 0xfff8, 0xfffa, 0xfffc, 0xfffe]);
  });
}
await test("casemap skips exactly the two far-return words", () => {
  assert.deepEqual(callers(casemap), [0x350], "the casemap's own return is not an older caller");
  assert.equal(casemap.current.kind, "continuation");
  assert.equal(casemap.current.offset, 0x105);
  assert.deepEqual(casemap.stack.map(({ sp, word }) => [sp, word]),
    [[0xfffc, 0x353], [0xfffe, 0xffff]]);
  assert.equal(casemap.stackTruncated, false);
});
await test("ordinary instruction keeps the caller at the original SP", () => {
  assert.equal(direct.stack[0].sp, 0x8000);
  assert.deepEqual(callers(direct), [0x350]);
});
for (const [name, value, firstSP] of [["INT", wrappedINT, 4], ["casemap", wrappedCasemap, 2]]) {
  await test(`${name} frame skip wraps at the stack segment boundary`, () => {
    assert.equal(value.raw.sp, 0xfffe, "raw CPU registers remain unchanged");
    assert.equal(value.stack[0].sp, firstSP);
    assert.deepEqual(callers(value), [0x350]);
    assert.equal(value.stack.length, 1024);
    assert.equal(value.stackTruncated, true);
  });
}
