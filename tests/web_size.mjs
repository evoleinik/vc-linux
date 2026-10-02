// Sum separate HTTP gzip payloads, including the page, transitive JS/CSS,
// font and preloaded wasm. Counting only vc.wasm hides a sizeable xterm cost.
// No browser or HTTP server is needed: follow the built page's local URLs.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, relative, resolve, sep } from 'node:path';
import { gzipSync } from 'node:zlib';

const root = resolve(process.argv[2] || 'build/web');
const pending = ['index.html'];
const sizes = new Map();

function add(from, url) {
  if (/^(?:[a-z]+:|\/\/|#)/i.test(url)) return;
  const path = decodeURIComponent(url.split(/[?#]/)[0]);
  const file = relative(root, resolve(root, dirname(from), path));
  assert.ok(file && file !== '..' && !file.startsWith(`..${sep}`), `asset escapes build: ${url}`);
  if (!sizes.has(file)) pending.push(file);
}

while (pending.length) {
  const file = pending.pop();
  if (sizes.has(file)) continue;
  const bytes = readFileSync(resolve(root, file));
  sizes.set(file, gzipSync(bytes, { level: 9 }).length);
  const text = bytes.toString('utf8');
  if (/\.html$/.test(file)) {
    for (const [tag] of text.matchAll(/<(?:link|script)\b[^>]*>/g)) {
      const url = tag.match(/(?:href|src)=["']([^"']+)["']/)?.[1];
      if (url) add(file, url);
    }
  }
  if (/\.(?:html|css)$/.test(file)) {
    for (const [, url] of text.matchAll(/url\(["']?([^"')]+)["']?\)/g)) add(file, url);
  }
  if (/\.(?:m?js)$/.test(file)) {
    for (const [, url] of text.matchAll(/\b(?:import|export)\s+(?:[^;]*?\s+from\s*)?["']([^"']+)["']/g))
      add(file, url);
  }
}

assert.ok(sizes.has('vc.wasm'), 'the main wasm must be included in the first-load budget');
const total = [...sizes.values()].reduce((sum, size) => sum + size, 0);
for (const [file, size] of [...sizes].sort()) console.log(`  ${file}: ${size.toLocaleString('en-US')} gzip bytes`);
console.log(`web first load: ${total.toLocaleString('en-US')} gzip bytes (${(total / 1_000_000).toFixed(3)} MB; limit 1,300,000)`);
assert.ok(total <= 1_300_000, `first load ${total} exceeds 1.3 MB`);
const beforeLazyFiles = 1_299_288;
const reduction = beforeLazyFiles - total;
console.log(`web lazy-file reduction: ${beforeLazyFiles.toLocaleString('en-US')} -> ${total.toLocaleString('en-US')} gzip bytes; saved ${reduction.toLocaleString('en-US')} (required 150,000)`);
assert.ok(reduction >= 150_000, `lazy H: files saved ${reduction} bytes; need at least 150000`);
