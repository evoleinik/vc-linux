// Every file the page loads from this build must carry the same ?v= build
// hash. Otherwise a browser holding cached files from an older build can
// pair an old loader with a new vc.wasm, and VC fails to start.
//   node tests/web_assets.mjs build/web
import { readFileSync } from "node:fs";
import { join } from "node:path";

const dir = process.argv[2] || "build/web";
const built = ["vc.mjs", "vc.wasm", "vc-web.js", "speaker.js"];
const sources = {
  "index.html": readFileSync(join(dir, "index.html"), "utf8"),
  "vc-web.js": readFileSync(join(dir, "vc-web.js"), "utf8"),
};
const failures = [];
const versions = new Set();
for (const [file, text] of Object.entries(sources)) {
  if (text.includes("__V__")) failures.push(`${file}: placeholder __V__ was not replaced`);
  for (const name of built) {
    const pattern = new RegExp(`\\./${name.replace(".", "\\.")}(\\?v=([0-9a-f]+))?(?=["'\`])`, "g");
    for (const match of text.matchAll(pattern)) {
      if (!match[2]) failures.push(`${file}: ./${name} has no ?v= build hash`);
      else versions.add(match[2]);
    }
  }
}
// vc.wasm is fetched through locateFile, which must append the same hash.
if (!/locateFile:[^\n]*\?v=([0-9a-f]+)/.test(sources["vc-web.js"]))
  failures.push("vc-web.js: locateFile does not append ?v= to vc.wasm");
else versions.add(sources["vc-web.js"].match(/locateFile:[^\n]*\?v=([0-9a-f]+)/)[1]);
// Any-motion tracking (1003), so VC's own mouse cursor follows the pointer
// without a click. Button-event tracking (1002) reports motion only while held.
if (!sources["vc-web.js"].includes("\\x1b[?1003h"))
  failures.push("vc-web.js: does not enable any-motion mouse tracking (?1003h)");
if (versions.size !== 1) failures.push(`expected one build hash, found: ${[...versions].join(", ") || "none"}`);

if (failures.length) {
  console.error("web assets: FAIL\n  " + failures.join("\n  ") + "\nFix: build with `make web`, which stamps the hash.");
  process.exit(1);
}
console.log(`web assets: every page asset carries ?v=${[...versions][0]}`);
