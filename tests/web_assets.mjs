// Startup assets keep the ?v= build hash. Lazy side modules need a hash in
// their actual filename: GitHub Pages ignores query strings across deploys.
//   node tests/web_assets.mjs build/web
import { readFileSync, readdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { join } from "node:path";
import { runInNewContext } from "node:vm";

const dir = process.argv[2] || "build/web";
const sources = Object.fromEntries([
  "index.html", "vc-web.js", "vc-layout.js", "vc-source.js", "vc-keypad.js", "vc-language.js", "speaker.js", "graphics.js",
].map(file => [file, readFileSync(join(dir, file), "utf8")]));
const failures = [];
const versions = new Set();
const referenced = new Set();
const programs = ["gwbasic", "bootlogo", "rogue", "vz"];
const sideFiles = readdirSync(dir).filter(file => file.endsWith(".wasm") && file !== "vc.wasm").sort();
const sideHashes = new Set();
for (const program of programs) {
  const names = sideFiles.filter(file => file.startsWith(`${program}.`));
  if (names.length !== 1) failures.push(`${program}: expected exactly one published side module, found ${names.length}`);
}
for (const file of sideFiles) {
  const match = file.match(/^(gwbasic|bootlogo|rogue|vz)\.([0-9a-f]{12})\.wasm$/);
  if (!match) failures.push(`${file}: side module filename has no build hash`);
  else sideHashes.add(match[2]);
}
if (sideHashes.size !== 1) failures.push("side modules must carry one shared filename build hash");
else if (sideFiles.length === programs.length) {
  // Independently verify the generation fingerprint against the shipped
  // bytes: a merely hash-looking constant would not prevent version mixing.
  const version = [...sideHashes][0];
  const digest = createHash("sha256");
  let complete = true;
  for (const program of programs) {
    const name = `${program}.${version}.wasm`;
    if (!sideFiles.includes(name)) { complete = false; break; }
    const bytes = readFileSync(join(dir, name));
    const length = Buffer.alloc(8);
    length.writeBigUInt64LE(BigInt(bytes.length));
    digest.update(`${program}\0`, "ascii").update(length).update(bytes);
  }
  if (complete && digest.digest("hex").slice(0, 12) !== version)
    failures.push("side module filename hash does not match the published bytes");
}
// Read the names retained in the actual main binary, not the C source or a
// separate manifest: a header/main rebuild missed by make must fail here.
// C strings end at NUL; the smoke test additionally checks actual EXEC URLs.
const main = readFileSync(join(dir, "vc.wasm")).toString("latin1");
const requestedSides = [...new Set([...main.matchAll(/(?:gwbasic|bootlogo|rogue|vz)(?:\.[A-Za-z0-9_-]+)?\.wasm(?=\0)/g)]
  .map(([file]) => file))].sort();
if (requestedSides.length !== programs.length || JSON.stringify(requestedSides) !== JSON.stringify(sideFiles))
  failures.push(`main requests [${requestedSides.join(", ")}], but published files are [${sideFiles.join(", ")}]`);
for (const [file, text] of Object.entries(sources)) {
  if (text.includes("__V__")) failures.push(`${file}: placeholder __V__ was not replaced`);
  // Include vendor assets and future relative imports, not just a fixed
  // allow-list of filenames. Anchored licence links are stamped as well.
  for (const [, path] of text.matchAll(/["'`]((?:\.\/)[^"'`\s$]+)["'`]/g)) {
    const url = new URL(path, "https://vc.invalid/");
    const version = url.searchParams.get("v");
    if (!/^[0-9a-f]{12}$/.test(version || "")) failures.push(`${file}: ${path} has no build hash`);
    else versions.add(version);
    const name = decodeURIComponent(url.pathname.slice(1));
    referenced.add(name);
    try { readFileSync(join(dir, name)); }
    catch { failures.push(`${file}: ${path} does not exist`); }
  }
}
// Exercise the actual built page's resolver for main AND lazy wasm. The C
// loader calls this same locateFile before its fetch (also checked in smoke).
const resolver = sources["vc-web.js"].match(/locateFile:\s*(.+),\s*$/m)?.[1];
if (!resolver) failures.push("vc-web.js: missing locateFile resolver");
else {
  const locateFile = runInNewContext(`(${resolver})`);
  for (const file of ["vc.wasm", ...requestedSides]) {
    const url = new URL(locateFile(file, "https://vc.invalid/subdir/"));
    if (url.pathname !== `/subdir/${file}` || !/^[0-9a-f]{12}$/.test(url.searchParams.get("v") || ""))
      failures.push(`locateFile: ${file} lacks its subdirectory or build hash`);
    else versions.add(url.searchParams.get("v"));
    try { readFileSync(join(dir, file)); }
    catch { failures.push(`locateFile: ${file} does not exist`); }
    if (file !== "vc.wasm" && referenced.has(file)) {
      failures.push(`${file}: side modules must not be eagerly referenced by the page`);
    }
  }
}
// Any-motion tracking (1003), so VC's own mouse cursor follows the pointer
// without a click. Button-event tracking (1002) reports motion only while held.
if (!sources["vc-web.js"].includes("\\x1b[?1003h"))
  failures.push("vc-web.js: does not enable any-motion mouse tracking (?1003h)");
if (versions.size !== 1) failures.push(`expected one build hash, found: ${[...versions].join(", ") || "none"}`);

if (failures.length) {
  console.error("web assets: FAIL\n  " + failures.join("\n  ") + "\nFix: build with `make web`, which stamps the hash.");
  process.exitCode = 1; // Let redirected diagnostics drain before exiting.
} else {
  console.log(`web assets: every page asset carries ?v=${[...versions][0]}; four hashed side filenames match the main binary (${[...sideHashes][0]})`);
}
