// Independent Node oracles for the published original-source maps. This parser
// reads JWasm's byte/source columns and binary-map table itself; it does not ask
// the Python map builder which instruction addresses ought to be present.
import assert from 'node:assert/strict';
import { readFile, mkdtemp, writeFile, rm } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join, resolve, basename } from 'node:path';
import { spawnSync } from 'node:child_process';

const root = resolve(import.meta.dirname, '..');
const output = resolve(process.argv[2] || process.env.WEB_OUT || join(root, 'build/web'));
const build = resolve(process.env.BUILD_DIR || join(root, 'build'));
const indexName = (await readFile(`${output}-work/source-index-name.txt`, 'utf8')).trim();
const physicalLines = text => text.split('\n').map(line => line.replace(/\r$/, ''));
const hash = data => createHash('sha256').update(data).digest('hex').slice(0, 12);

async function generated(name) {
  assert.equal(basename(name), name, `unsafe generated filename: ${name}`);
  const match = name.match(/\.([0-9a-f]{12})\.(?:json|txt)$/);
  assert.ok(match, `source asset lacks a content hash: ${name}`);
  const data = await readFile(join(output, name));
  assert.equal(match[1], hash(data), `source asset hash disagrees with bytes: ${name}`);
  return data;
}

function originalText(bytes, path = '') {
  try {
    return { text: new TextDecoder('utf-8', { fatal: true }).decode(bytes), encoding: 'utf-8' };
  } catch {
    // ICU's legacy converter swaps 1A/1C/7F; DOS source's ASCII controls
    // (notably its EOF byte) must remain byte-for-codepoint unchanged.
    const encoding = path.startsWith('third_party/vzeditor/') ? 'shift_jis' : 'ibm866';
    const decoder = new TextDecoder(encoding, { fatal: true });
    const text = bytes.toString('latin1').split(/([\x00-\x1f\x7f])/).map(part =>
      part.length === 1 && (part.charCodeAt(0) < 32 || part.charCodeAt(0) === 127)
        ? part : decoder.decode(Buffer.from(part, 'latin1'))).join('');
    return { text, encoding };
  }
}

const index = JSON.parse(await generated(indexName));
assert.equal(index.version, 1);
assert.deepEqual(Object.keys(index.images).sort(),
  ['VC.COM', 'VC.OVL', 'GWBASIC.EXE', 'LOGO.COM', 'ROGUE.EXE', 'VZ.COM', 'VC405.COM', 'VCSETUP.COM'].sort());
const maps = new Map();
const originals = new Map();
let fetchedBytes = 0;
const sourceURLs = new Set();
for (const [image, item] of Object.entries(index.images)) {
  const map = JSON.parse(await generated(item.url));
  assert.equal(map.version, 1);
  assert.equal(map.image, image);
  maps.set(image, map);
  if (map.kind !== 'asm') continue;
  for (const file of map.files) {
    const bytes = await readFile(join(root, file.path));
    const expected = originalText(bytes, file.path);
    const actual = await generated(file.url);
    assert.equal(file.name, basename(file.path));
    assert.equal(file.encoding, expected.encoding, `${file.path}: source encoding`);
    assert.ok(actual.toString('utf8') === expected.text, `${file.path}: original text, comments and line endings`);
    originals.set(file.path, physicalLines(expected.text));
    if (!sourceURLs.has(file.url)) {
      sourceURLs.add(file.url);
      fetchedBytes += actual.length;
    }
    for (let i = 0; i < (file.via || []).length; i++) {
      const parent = file.via[i];
      const child = file.via[i + 1] || file;
      const parentText = originalText(await readFile(join(root, parent.path)), parent.path).text;
      const line = physicalLines(parentText)[parent.line - 1];
      assert.equal(parent.name, basename(parent.path));
      assert.ok(line && new RegExp(`\\binclude\\s+${child.name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`, 'i').test(line),
        `${file.path}: include breadcrumb must name an actual parent INCLUDE line`);
    }
  }
  let previous = -1;
  for (const [offset, fileIndex, line, length] of map.lines) {
    assert.ok(Number.isInteger(offset) && offset > previous, `${image}: duplicate/unsorted instruction mapping at ${offset}`);
    previous = offset;
    const file = map.files[fileIndex];
    assert.ok(file, `${image}+${offset}: unknown source file index`);
    assert.ok(Number.isInteger(line) && line >= 1 && line <= originals.get(file.path).length,
      `${image}+${offset}: original line number out of range`);
    assert.ok(Number.isInteger(length) && length > 0 && length <= 15,
      `${image}+${offset}: impossible instruction length`);
  }
  assert.ok(map.lines.length > 100, `${image}: assembly image unexpectedly unmapped`);
}

const mnemonics = new Set(`AAA AAD AAM AAS ADC ADD AND CALL CBW CLC CLD CLI CMC CMP CMPS CMPSB CMPSW CWD
  DAA DAS DEC DIV ESC HLT IDIV IMUL IN INC INT INTO IRET JA JAE JB JBE JC JCXZ JE JG JGE JL JLE
  JMP JNA JNAE JNB JNBE JNC JNE JNG JNGE JNL JNLE JNO JNP JNS JNZ JO JP JPE JPO JS JZ
  LAHF LDS LEA LES LOCK LODS LODSB LODSW LOOP LOOPE LOOPNE LOOPNZ LOOPZ MOV MOVS MOVSB MOVSW
  MUL NEG NOP NOT OR OUT POP POPF PUSH PUSHF RCL RCR REP REPE REPNE REPNZ REPZ RET RETF RETN
  ROL ROR SAHF SAL SAR SBB SCAS SCASB SCASW SHL SHR STC STD STI STOS STOSB STOSW SUB TEST
  WAIT XCHG XLAT XLATB XOR`.split(/\s+/));

function instructionSource(text) {
  let statement = text.split(';', 1)[0].trim();
  statement = statement.replace(/^[\w@?$\.]+::?\s*/, '');
  return statement.split(/\s+/, 1)[0].toUpperCase();
}

async function listedInstructions(image) {
  // Latin-1 is intentional here: fixed listing columns count bytes, not Unicode
  // characters. Source-text comparisons below use the independent file decoder.
  const vc405 = image === 'VC405.COM' || image === 'VCSETUP.COM';
  const filename = image === 'VC405.COM' ? 'VC.COM' : image;
  const text = (await readFile(join(build, vc405 ? 'vc405' : 'gen', `${filename}.lst`))).toString('latin1');
  const imageBytes = await readFile(join(build, vc405 ? 'vc405' : '', filename));
  const executable = imageBytes[0] === 0x4d && imageBytes[1] === 0x5a;
  const header = executable ? imageBytes.readUInt16LE(8) * 16 : 0;
  const segments = new Map();
  const metadata = text.split('Segments and Groups:', 2)[1].split('Procedures,', 1)[0];
  for (const line of physicalLines(metadata)) {
    const match = line.match(/^(\S+)\s+(?:\.\s+)*16 Bit\s+([0-9A-F]+)\s+\S+\s+\S+\s+'([^']+)'/);
    if (match) segments.set(match[1], { length: parseInt(match[2], 16), code: match[3] === 'CODE' });
  }
  for (const line of physicalLines(text.split('Binary Map:', 2)[1].split('Macros:', 1)[0])) {
    const match = line.match(/^(\S+)\s+([0-9A-F]+)\s+([0-9A-F]+)\s+([0-9A-F]+)\s+([0-9A-F]+)\s*$/);
    if (!match || !segments.has(match[1])) continue;
    const segment = segments.get(match[1]);
    const file = parseInt(match[2], 16), size = parseInt(match[4], 16);
    // COM's initial ORG is included in Length but not in the file allocation.
    segment.base = file - header - (executable ? 0 : segment.length - size);
  }
  let segment;
  const result = [];
  for (const [rowIndex, raw] of physicalLines(text.split('Binary Map:', 1)[0]).entries()) {
    if (raw.length < 32 || /^\s*>/.test(raw)) continue;
    const source = raw.slice(32);
    const declaration = source.trim().match(/^(\S+)\s+(SEGMENT|ENDS)\b/i);
    if (declaration) {
      if (declaration[2].toUpperCase() === 'SEGMENT') segment = declaration[1];
      else if (segment === declaration[1]) segment = undefined;
    }
    const code = source.trim().match(/^\.CODE(?:\s+(\S+))?/i);
    if (code) segment = code[1] || [...segments.keys()].find(name => name.endsWith('_TEXT'));
    const data = source.trim().match(/^\.(DATA\??|CONST|FARDATA\??)\b/i);
    if (data) segment = vc405 ? { DATA: '_DATA', 'DATA?': '_BSS', CONST: 'CONST' }[data[1].toUpperCase()] : undefined;
    const address = raw.match(/^([0-9A-F]{4,8}) /);
    // 4.05 intentionally puts its Alloc0 startup instructions in CONST.
    // Listing mnemonics establish their boundaries just as in _TEXT.
    if (!address || !segments.has(segment) || (!vc405 && !segments.get(segment).code)) continue;
    const field = raw.slice(address[0].length, 28).trim();
    if (!field || field.startsWith('=')) continue;
    const mnemonic = instructionSource(source);
    if (!mnemonics.has(mnemonic)) continue;
    let length = 0;
    for (const token of field.split(/\s+/)) {
      assert.match(token, /^(?:[0-9A-Fa-f]{2})+[osri]?$/, `${image}: unknown listing byte column`);
      length += token.replace(/[osri]$/, '').length / 2;
    }
    const offset = segments.get(segment).base + parseInt(address[1], 16);
    const generated = raw[28] === '*' || /\d/.test(raw.slice(28, 30));
    result.push({ offset, mnemonic, length, source, generated, listingLine: rowIndex + 1,
      bytes: imageBytes.subarray(header + offset, header + offset + length) });
  }
  const minimum = { 'VC.COM': 2500, 'VC.OVL': 35000, 'VC405.COM': 22000, 'VCSETUP.COM': 4700 };
  assert.ok(result.length > minimum[image], `${image}: independent listing parser lost instructions`);
  return result;
}

// Literal source/listing witnesses, independent of the mapper. Include both
// plain USES and delayed LOCAL prologues, then epilogues which must stay on RET.
// Input0's nine setup rows are the exact regression from Brief 25.
const generatedSamples = {
  'VC.COM': [
    [1477, 1478, 'asm/VC.ASM', 719, /^SetBreak\s+PROC\b/],
    [1493, 1495, 'asm/VC.ASM', 733, /^\s*RET\s*$/],
  ],
  'VC.OVL': [
    [29449, 29457, 'asm/VCKEYB.INC', 169, /^Input0\s+PROC\s+FAR\s+USES\b/],
    [17857, 17864, 'asm/VCLABEL.INC', 278, /^GetVol\s+PROC\b/],
    [23478, 23486, 'asm/VCVIEW.INC', 2026, /^ViewPutLine\s+PROC\b/],
    [29858, 29866, 'asm/VCKEYB.INC', 597, /^\s*@@Exit:\s*RET\s*$/],
  ],
  'VC405.COM': [
    [24399, 24407, 'asm405/VCSUB2.INC', 1680, /^Input0\s+PROC\s+C\s+NEAR\b/],
  ],
  'VCSETUP.COM': [
    [5783, 5791, 'asm405/VCSTSUB.INC', 2240, /^Input0\s+PROC\s+C\s+NEAR\b/],
  ],
};

for (const image of ['VC.COM', 'VC.OVL', 'VC405.COM', 'VCSETUP.COM']) {
  const map = maps.get(image), listed = await listedInstructions(image);
  if (image === 'VC405.COM' || image === 'VCSETUP.COM') {
    assert.ok(map.files.every(file => file.path.startsWith('asm405/')),
      `${image}: every Source location belongs to the unedited 4.05 source tree`);
  }
  const addresses = new Map(map.lines.map(record => [record[0], record]));
  for (const row of listed) {
    assert.ok(addresses.has(row.offset), `${image}+0x${row.offset.toString(16)}: listed instruction has no source mapping`);
  }
  // Ten deterministic addresses spread through each image, chosen from the
  // LISTING before looking at the map. Do not filter away a failed mapping.
  const ordinary = listed.filter(row => !row.generated);
  for (let i = 0; i < 10; i++) {
    const row = ordinary[Math.floor(i * (ordinary.length - 1) / 9)];
    const [, fileIndex, line] = addresses.get(row.offset);
    const file = map.files[fileIndex];
    const actual = originals.get(file.path)[line - 1];
    assert.equal(instructionSource(actual), row.mnemonic,
      `${image}+0x${row.offset.toString(16)} -> ${file.name}:${line}: sample lost its original mnemonic (${row.mnemonic})`);
  }
  const listingRows = new Map(listed.map(row => [row.listingLine, row]));
  let generatedCount = 0;
  for (const [first, last, path, expectedLine, expectedText] of generatedSamples[image] || []) {
    assert.match(originals.get(path)[expectedLine - 1], expectedText, `${path}:${expectedLine}: generated-row fixture changed`);
    for (let listingLine = first; listingLine <= last; listingLine++) {
      const row = listingRows.get(listingLine);
      assert.ok(row?.generated, `${image}.lst:${listingLine}: sample must be a generated instruction`);
      const [, fileIndex, line] = addresses.get(row.offset);
      const file = map.files[fileIndex];
      assert.deepEqual([file.path, line], [path, expectedLine],
        `${image}.lst:${listingLine} generated ${row.source.trim()} -> ${file.name}:${line}: expected ${basename(path)}:${expectedLine}`);
      generatedCount++;
    }
  }
  const expectedCalls = listed.filter(row => row.mnemonic === 'CALL').map(row => {
    // JWasm explicitly lists PUSH CS + E8 as a same-segment far CALL.
    const optimized = row.bytes[0] === 0x0e && row.bytes[1] === 0xe8;
    let opcode = 0;
    while ([0x26, 0x2e, 0x36, 0x3e, 0xf0, 0xf2, 0xf3].includes(row.bytes[opcode])) opcode++;
    const far = optimized || row.bytes[opcode] === 0x9a || row.bytes[opcode] === 0xff && (row.bytes[opcode + 1] & 0x38) === 0x18;
    return [row.offset + row.length, row.offset + (optimized ? 1 : 0), far ? 'far' : 'near'];
  }).sort((a, b) => a[1] - b[1]);
  assert.deepEqual(map.calls, expectedCalls, `${image}: return metadata must contain exactly listing CALL ends`);
  console.log(`source maps: ${image}: every ${listed.length.toLocaleString('en')} listed instruction maps once; 10 mnemonic samples; ${generatedCount} generated samples; ${expectedCalls.length} CALL ends`);
}

// Rogue is a separate provenance class. Every published function must be a
// CODE address and a symbol literally present in the linker's memory map.
const rogue = maps.get('ROGUE.EXE');
assert.equal(rogue.kind, 'functions');
assert.equal(rogue.lines, undefined, 'compiled C must not claim invented C line numbers');
assert.equal(rogue.files, undefined, 'compiled C has function symbols, not source-line mappings');
const rogueMap = await readFile(join(build, 'rogue/ROGUE.MAP'), 'utf8');
const symbols = new Set();
for (const line of rogueMap.split('|   Memory Map   |', 2)[1].split('|   Module Segments   |', 1)[0].split('\n')) {
  const match = line.trim().match(/^([\da-f]{4}):([\da-f]{4})[*+]?\s+(\S+)$/i);
  if (match) symbols.add(`${parseInt(match[1], 16) * 16 + parseInt(match[2], 16)}:${match[3]}`);
}
let end = 0;
for (const [start, next, name] of rogue.functions) {
  assert.ok(start >= end && next > start, 'compiled function ranges overlap');
  end = next;
  assert.ok(symbols.has(`${start}:${name}`), `Rogue invented a function: ${name}`);
}
assert.ok(rogue.functions.length > 200, 'Rogue lost its mapped functions');

// The current tree includes UTF-8 conversions. A CP866-only fixture prevents
// the required DOS fallback from becoming untested when every real file is
// temporarily valid UTF-8. The byte columns are generated by the same JWasm
// used for images, but the original bytes and expected line are the oracle.
const fixture = await mkdtemp(join(tmpdir(), 'vc-source-map-'));
try {
  const comment = Buffer.from([0x8f, 0xe0, 0xa8, 0xa2, 0xa5, 0xe2]); // Привет in CP866
  const source = Buffer.concat([Buffer.from('.model tiny\r\n.code\r\norg 100h\r\nstart:\tmov ax,1 ; '), comment, Buffer.from('\r\n\tret\r\nend start\r\n')]);
  await writeFile(join(fixture, 'FIXTURE.ASM'), source);
  const assemble = spawnSync(join(root, 'tools/jwasm/jwasm'), ['-q', '-Sg', '-bin', '-Fl=FIXTURE.lst', '-Fo=FIXTURE.COM', 'FIXTURE.ASM'], { cwd: fixture, encoding: 'utf8' });
  assert.equal(assemble.status, 0, assemble.stdout + assemble.stderr);
  const program = `import json,sys\nfrom pathlib import Path\nfrom tools.source_maps import Sources,jwasm_origins,decode_source\np=Path(sys.argv[1])\no=jwasm_origins(p/'FIXTURE.lst',Sources(p))\nt,e=decode_source((p/'FIXTURE.ASM').read_bytes())\nprint(json.dumps({'text':t,'encoding':e,'lines':[x.line for x in o.values()]}))\n`;
  const run = spawnSync(join(root, '.venv/bin/python'), ['-c', program, fixture], { cwd: root, encoding: 'utf8' });
  assert.equal(run.status, 0, run.stdout + run.stderr);
  const value = JSON.parse(run.stdout);
  assert.equal(value.encoding, 'ibm866');
  assert.equal(value.text, originalText(source).text, 'CP866 fallback did not preserve the original comment');
  assert.match(physicalLines(value.text)[3], /mov ax,1 ; Привет$/);
  assert.ok(value.lines.includes(4), 'CP866 instruction no longer maps to original physical line 4');
} finally {
  await rm(fixture, { recursive: true, force: true });
}

console.log(`source maps: exact original text/CP866, include breadcrumbs, immutable URLs, all eight images; ${fetchedBytes.toLocaleString('en')} source-text bytes (${sourceURLs.size} lazy files)`);
