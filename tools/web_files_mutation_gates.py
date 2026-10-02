"""Reproduce the thirteen Brief 32 lazy-file mutation reds and restored greens.

Run after `make web` with emcc on PATH and EM_CACHE pointing at the local
build cache. Every mutation lives in a private temporary source copy; neither
production sources nor the site's wasm are changed. Each red is recompiled
after restoring the same private copy, and that identical gate must pass.
"""
from dataclasses import dataclass
import argparse
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PYTHON_CASES = {
    "publisher_hash": ("tools/web_files.py", "tests/test_web_files.py::test_changed_inputs_are_refused_before_publication[source]",
                       "DID NOT RAISE", "trust the manifest hash instead of hashing the actual source bytes",
                       (("digest = sha256(data).hexdigest()", 'digest = entry["sha256"]'),)),
    "embedded_name": ("tools/embed.py", "tests/test_embed.py::test_lazy_file_bytes_only_leave_the_browser_binary[web-metadata-only]",
                      "AssertionError", "derive the immutable filename from the wrong SHA-256 slice",
                      (('f"file.{digest[:12]}.bin"', 'f"file.{digest[12:24]}.bin"'),)),
    "eager_bytes": ("tools/embed.py", "tests/test_embed.py::test_lazy_file_bytes_only_leave_the_browser_binary[web-metadata-only]",
                    "AssertionError", "force lazy executable/source contents back into the browser binary",
                    (("        names.add(name)\n", "        names.add(name)\n        lazy = False\n"),)),
}
RUNTIME_CASES = {
    "cache": ("runtime/web_files.c", "actual: 2", "disable successful reference and identical-file caching", (
        ("if (reference->data) return 0;", "if (0 && reference->data) return 0;"),
        ("if (other->data && other->sha256 && other->size == reference->size &&",
         "if (0 && other->data && other->sha256 && other->size == reference->size &&"))),
    "private_reference": ("runtime/web_files.c", "the complete private reference is not the edited DOS file",
                          "alias mutable MEMFS contents to the private EXEC reference", (
        ("        const contents = new Uint8Array(size);\n        contents.set(HEAPU8.subarray(data, data + size));",
         "        const contents = HEAPU8.subarray(data, data + size);"),)),
    "stale_response": ("runtime/web_files.c", "a stale response cannot overwrite replacement or edited content",
                       "remove the inode/marker check before publishing a suspended download", (
        ("if (node.id !== inode || node.vcLazyFile !== reference) return 5;",
         "if (false && (node.id !== inode || node.vcLazyFile !== reference)) return 5;"),)),
    "staged_checksum": ("runtime/web_files.c", "stage corruption must be a DOS error",
                        "remove the post-staging Adler32 guard (SHA-256 still verifies the original response)", (
        ("embed_adler32(bytes, reference->size) == reference->checksum) error = 0;", "1) error = 0;"),)),
    "metadata_size": ("runtime/web_files.c", "real size before first open", "add one to the advertised lazy-file length", (
        ("            node.usedBytes = size;", "            node.usedBytes = size + 1;"),)),
    "metadata_date": ("runtime/web_files.c", "real creation date before first open",
                      "replace MEMFS creation-time mtime with an incorrect epoch date", (
        ("            node.usedBytes = size;", "            node.usedBytes = size;\n            node.mtime = 0;"),)),
    "truncate": ("runtime/web_files.c", "intentional truncation does not fetch the discarded contents",
                 "keep the lazy marker after deliberate truncation", (
        ("if (attributes.size !== undefined) delete node.vcLazyFile;",
         "if (false && attributes.size !== undefined) delete node.vcLazyFile;"),)),
    "full_sha256": ("runtime/web_programs.c", "same-Adler32 corruption are DOS errors",
                    "skip the full SHA-256 comparison and accept a same-size Adler32 collision", (
        ("if (actualHash !== expectedHash)", "if (false && actualHash !== expectedHash)"),)),
    "hash_snapshot": ("runtime/web_programs.c", "Uint8Array(6)",
                      "hash a transport-owned buffer that changes before staging", (
        ("bytes = new Uint8Array(bytes);", "bytes = bytes;"),)),
    "digest_deadline": ("runtime/web_programs.c", "unbounded digest wait",
                        "cancel the download deadline before asynchronous SHA-256 finishes", (
        ("            if (expectedHash) {", "            if (expectedHash) {\n                clearTimeout(timer);"),)),
}
ORDER = ("cache", "private_reference", "stale_response", "staged_checksum", "metadata_size", "metadata_date", "truncate",
         "publisher_hash", "embedded_name", "eager_bytes", "full_sha256", "hash_snapshot", "digest_deadline")

# After full SHA verification was added, an ordinary corrupt response is
# refused before the C staging check. Exercise that original guard at its
# remaining boundary: deliberately corrupt only the private staging write.
STAGING_GATE = r'''
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
const path = process.argv[2];
const {default:createFixture} = await import(pathToFileURL(path));
const original = Uint8Array.of(65,98,121,116,101,115);
const fixture = await createFixture({
  wasmBinary:readFileSync(path.replace(/\.mjs$/, '.wasm')),
  vcFetchProgram:async()=>original,
});
fixture.ccall('fixture_init', null, [], []);
fixture.ccall('fixture_install','number',['number','string'],[0,'/stage-test.txt']);
const write = fixture.FS.writeFile;
fixture.FS.writeFile = function(path, data, options) {
  if (path.startsWith('/var/vc/file-')) { data = new Uint8Array(data); data[0] ^= 1; }
  return write.call(this, path, data, options);
};
const open = () => fixture.ccall('fixture_open','number',['string','number'],['/stage-test.txt',0],{async:true});
assert.equal(await open(),5,'stage corruption must be a DOS error');
assert.throws(()=>fixture.FS.readFile('/stage-test.txt'));
fixture.FS.writeFile = write;
assert.equal(await open(),0);
assert.deepEqual(fixture.FS.readFile('/stage-test.txt'), original);
console.log('PASS staging-checksum: error, intact placeholder, restored retry');
'''


def replace_once(source: str, before: str, after: str) -> str:
    if source.count(before) != 1:
        raise ValueError(f"mutation anchor must occur exactly once: {before!r}")
    return source.replace(before, after, 1)


@dataclass
class Runner:
    emcc: str
    node: str
    work: Path
    log: object

    def record(self, message: str) -> None:
        self.log.write(message + "\n")
        self.log.flush()

    def run(self, command: list[str], *, want: int, diagnostic: str = "") -> None:
        self.record("$ " + shlex.join(map(str, command)))
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        self.record(result.stdout.rstrip())
        self.record(f"exit={result.returncode}")
        if result.returncode != want or diagnostic not in result.stdout:
            print(result.stdout, flush=True)
            raise RuntimeError(f"expected exit {want} and {diagnostic!r}")

    def compile(self, directory: Path, changed: str, copied: Path) -> Path:
        sources = ["tests/web_files_fixture.c", "runtime/web_files.c", "runtime/web_programs.c",
                   "runtime/web_sha256.c", "runtime/embed_lzma.c"]
        output = directory / "files-test.mjs"
        self.run([self.emcc, "-O2", "-flto", "-sASYNCIFY", "-sASYNCIFY_IGNORE_INDIRECT=1",
                  "-sMODULARIZE", "-sEXPORT_ES6", "-sENVIRONMENT=node",
                  '-sEXPORTED_RUNTIME_METHODS=["FS","ccall"]',
                  '-sEXPORTED_FUNCTIONS=["_fixture_init","_fixture_install","_fixture_open","_fixture_reference","_malloc","_free"]',
                  "--no-entry", "-Iruntime", f"-I{self.work}",
                  *(str(copied) if name == changed else name for name in sources), "-o", str(output)], want=0)
        return output

    def runtime_case(self, name: str) -> None:
        path, diagnostic, description, replacements = RUNTIME_CASES[name]
        source = (ROOT / path).read_text()
        mutant = source
        for before, after in replacements:
            mutant = replace_once(mutant, before, after)
        with tempfile.TemporaryDirectory(prefix=f"vc405-lazy-{name}-") as temporary:
            directory = Path(temporary)
            copied = directory / Path(path).name
            copied.write_text(mutant)
            gate = ROOT / "tests/test_web_files.mjs"
            if name == "staged_checksum":
                gate = directory / "staged-checksum.mjs"
                gate.write_text(STAGING_GATE)
            self.record(f"\nDEFECT {name}: {description}")
            output = self.compile(directory, path, copied)
            self.run([self.node, str(gate), str(output)], want=1, diagnostic=diagnostic)
            self.record(f"RED {name}: expected assertion observed")
            copied.write_text(source)
            assert copied.read_text() == source
            output = self.compile(directory, path, copied)
            self.run([self.node, str(gate), str(output)], want=0, diagnostic="PASS" if name == "staged_checksum" else "passed")
            self.record(f"RESTORED GREEN {name}: private source restored byte-for-byte and gate rerun")

    def python_case(self, name: str) -> None:
        path, test, diagnostic, description, replacements = PYTHON_CASES[name]
        source = (ROOT / path).read_text()
        # The relocated script must still resolve real repository imports.
        source = replace_once(source, "ROOT = Path(__file__).resolve().parents[1]", f"ROOT = Path({str(ROOT)!r})")
        mutant = source
        for before, after in replacements:
            mutant = replace_once(mutant, before, after)
        with tempfile.TemporaryDirectory(prefix=f"vc405-lazy-{name}-") as temporary:
            copied = Path(temporary) / Path(path).name
            copied.write_text(mutant)
            if name == "publisher_hash":
                code = ("import importlib.util, sys, pytest; "
                        f"spec=importlib.util.spec_from_file_location('tools.web_files',{str(copied)!r}); "
                        "module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module); "
                        "sys.modules['tools.web_files']=module; "
                        f"raise SystemExit(pytest.main(['-q',{test!r}]))")
            else:
                code = ("import subprocess, pytest; original=subprocess.run; "
                        "subprocess.run=lambda args,*a,**kw: original("
                        f"[args[0],{str(copied)!r},*args[2:]] "
                        "if len(args)>1 and str(args[1]).endswith('/tools/embed.py') else args,*a,**kw); "
                        f"raise SystemExit(pytest.main(['-q',{test!r}]))")
            self.record(f"\nDEFECT {name}: {description}")
            command = [sys.executable, "-c", code]
            self.run(command, want=1, diagnostic=diagnostic)
            self.record(f"RED {name}: expected assertion observed")
            copied.write_text(source)
            assert copied.read_text() == source
            self.run(command, want=0, diagnostic="1 passed")
            self.record(f"RESTORED GREEN {name}: relocated source restored and identical test rerun")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emcc", default="emcc")
    parser.add_argument("--node", default="node")
    parser.add_argument("--work", type=Path, default=ROOT / "build/web-work")
    parser.add_argument("--output", type=Path, default=ROOT / "build/gates-brief32-lazy-mutations.log")
    parser.add_argument("--case", choices=ORDER, action="append")
    arguments = parser.parse_args()
    if not (arguments.work / "web_program_names.h").is_file():
        parser.error("build the web module-name header with make web first")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    cases = arguments.case or ORDER
    with arguments.output.open("w") as log:
        runner = Runner(arguments.emcc, arguments.node, arguments.work, log)
        runner.record("Brief 32 lazy-file mutations: isolated source copies, no network/browser; timeout 60s per command")
        runner.record("Reproduce: " + shlex.join([sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]]))
        runner.run([arguments.node, "--version"], want=0)
        for name in cases:
            if name in RUNTIME_CASES:
                runner.runtime_case(name)
            else:
                runner.python_case(name)
            print(f"RED -> RESTORED GREEN {name}", flush=True)
        runner.record(f"ALL {len(cases)} PLANTED DEFECTS RED; ALL {len(cases)} RESTORED RERUNS GREEN")
    print(f"Evidence: {arguments.output}", flush=True)


if __name__ == "__main__":
    main()
