// Inspect the shipped binaries, not Makefile strings: each generated program
// is a non-suspending side module sharing VC's CPU, memory and function table.
//   node tests/web_modules.mjs build/web
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { join } from 'node:path';

const directory = process.argv[2] || 'build/web';
const published = await readdir(directory);
const moduleSizes = new Map();
const readModule = async (name) => {
  const names = name === 'vc' ? ['vc.wasm'] :
    published.filter(file => new RegExp(`^${name}\\.[0-9a-f]{12}\\.wasm$`).test(file));
  assert.equal(names.length, 1, `${name}: exactly one versioned side module is published`);
  const bytes = await readFile(join(directory, names[0]));
  moduleSizes.set(name, bytes.length);
  return WebAssembly.compile(bytes);
};

function dependencies(module, label, fileSize) {
  const sections = WebAssembly.Module.customSections(module, 'dylink.0');
  assert.equal(sections.length, 1, `${label} is an Emscripten dynamically linkable module`);
  const bytes = new Uint8Array(sections[0]);
  let offset = 0;
  const number = () => {
    let result = 0;
    for (let shift = 0; shift < 35; shift += 7) {
      assert.ok(offset < bytes.length, `${label}: bounded dylink integer`);
      const byte = bytes[offset++];
      result += (byte & 127) * 2 ** shift;
      if (!(byte & 128)) return result;
    }
    assert.fail(`${label}: oversized dylink integer`);
  };
  const needed = [];
  let memoryDescription = false;
  while (offset < bytes.length) {
    const kind = bytes[offset++];
    const size = number();
    const end = offset + size;
    assert.ok(end <= bytes.length, `${label}: bounded dylink subsection`);
    if (kind === 1) {
      const memorySize = number();
      const memoryAlign = number();
      number(); // table size
      number(); // table alignment
      assert.equal(offset, end, `${label}: complete dylink memory subsection`);
      assert.ok(!memoryDescription, `${label}: exactly one memory subsection`);
      memoryDescription = true;
      if (fileSize !== undefined) {
        assert.ok(memoryAlign < 32, `${label}: bounded wasm32 memory alignment`);
        assert.ok(memorySize + 2 ** memoryAlign <= fileSize,
          `${label}: side statics plus alignment fit within one file-size allowance ` +
          `(${memorySize} + ${2 ** memoryAlign} <= ${fileSize})`);
      }
    } else if (kind === 2) {
      const count = number();
      for (let index = 0; index < count; index++) {
        const length = number();
        assert.ok(offset + length <= end, `${label}: bounded dylink dependency name`);
        needed.push(new TextDecoder().decode(bytes.subarray(offset, offset + length)));
        offset += length;
      }
      assert.equal(offset, end, `${label}: complete dylink dependency subsection`);
    }
    offset = end;
  }
  assert.ok(memoryDescription, `${label}: describes its dylink static-memory requirement`);
  return needed;
}

const main = await readModule('vc');
assert.deepEqual(dependencies(main, 'vc.wasm'), [], 'main must not eagerly load any side library');
const mainExports = new Map(WebAssembly.Module.exports(main).map(({ name, kind }) => [name, kind]));
const mainImports = WebAssembly.Module.imports(main);
assert.equal(mainExports.get('__main_argc_argv'), 'function', 'VC stays the main executable');
assert.ok(mainExports.size < 100, 'MAIN_MODULE=2 retains a small ABI instead of all libc');
for (const name of ['asyncify_start_unwind', 'asyncify_stop_unwind',
  'asyncify_start_rewind', 'asyncify_stop_rewind']) {
  assert.equal(mainExports.get(name), 'function', `main retains the direct Asyncify wait chain: ${name}`);
}
for (const name of ['__asyncjs__fetch_program', '_dlopen_js', 'emscripten_sleep']) {
  assert.ok(mainImports.some((entry) => entry.name === name), `main implements its direct ${name} wait`);
}

const common = ['cpu', 'cpu_int', 'flags_get', 'flags_set', 'mem', 'rt_budget', 'rt_code_delta'];
const programs = [
  ['gwbasic', ['rt_halted', 'port_in8', 'port_out8'], ['run_gwbasic_graphics']],
  ['bootlogo', [], []],
  ['rogue', [], []],
  ['vz', ['port_in8', 'port_out8', 'rt_fault'], []],
];
const linkerSymbols = new Set(['__memory_base', '__table_base', '__stack_pointer',
  '__indirect_function_table', 'memory']);
for (const [name, extraImports, extraExports] of programs) {
  const side = await readModule(name);
  assert.deepEqual(dependencies(side, `${name}.wasm`, moduleSizes.get(name)), [],
    `${name} must not fetch another library`);
  const imports = WebAssembly.Module.imports(side);
  const exports = WebAssembly.Module.exports(side);
  for (const entry of [...imports, ...exports]) {
    assert.doesNotMatch(entry.name, /asyncify|emscripten_sleep|__asyncjs/i,
      `${name}: translated side code must remain outside Asyncify`);
  }
  assert.deepEqual(exports.map(({ name: symbol }) => symbol).filter((symbol) => !symbol.startsWith('__')).sort(),
    [`image_${name}`, ...extraExports].sort(), `${name} exports only its own image and optional runner`);
  for (const entry of exports.filter(({ name: symbol }) => !symbol.startsWith('__'))) {
    assert.ok(!mainExports.has(entry.name), `${entry.name} must live only in its first-use side module`);
  }
  const hostImports = imports.filter((entry) => !linkerSymbols.has(entry.name));
  assert.deepEqual(hostImports.map(({ name: symbol }) => symbol).sort(), [...common, ...extraImports].sort(),
    `${name}: explicit shared CPU/runtime ABI`);
  for (const entry of hostImports) {
    assert.ok(['env', 'GOT.mem'].includes(entry.module), `${name}: no independent host instance`);
    assert.equal(mainExports.get(entry.name), entry.kind, `${name}: main exports ${entry.name}`);
  }
  assert.ok(imports.some((entry) => entry.name === 'memory' && entry.kind === 'memory'),
    `${name} shares the main linear memory`);
  assert.ok(imports.some((entry) => entry.name === '__indirect_function_table' && entry.kind === 'table'),
    `${name} shares the main indirect-call table`);
}
console.log('web modules: VC-only main, no eager libraries, shared host ABI, bounded side static memory, and four non-Asyncified sides passed');
