"""Brief 32: two real web/MEMFS behavioral defects, on private copies only.

Run with .venv/bin/python tools/vc405_web_behavior_mutations.py. The existing
Emscripten SDK/cache and all side modules are reused offline. Only a private
main is compiled, with the actual current Makefile flags and source list.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HOOK = b"if (door_root_fd < 0) return web_files_open(path, flags, mode);"
SYMBOL = b"image_vc405\0"
WRONG_SYMBOL = b"image_wrong\0"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web", type=Path, default=ROOT / "build/web")
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("EMSDK", ROOT.parent / "emsdk")))
    parser.add_argument("--evidence", type=Path, default=ROOT / "build/vc405-web-behavior-mutations")
    args = parser.parse_args()
    web = args.web.resolve()
    sdk = args.sdk.resolve()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    python = ROOT / ".venv/bin/python"
    node = sdk / "node/20.18.0_64bit/bin/node"
    compiler = sdk / "upstream/emscripten/emcc.py"
    for required in (python, node, compiler, sdk / ".emscripten", web / "vc.wasm"):
        if not required.is_file():
            raise FileNotFoundError(required)
    summary: list[str] = []

    def report(message: str) -> None:
        summary.append(message)
        print(message, flush=True)
        (evidence / "summary.log").write_text("\n".join(summary) + "\n")

    with tempfile.TemporaryDirectory(prefix="vc405-web-behavior-") as directory:
        temporary = Path(directory)
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1",
                           PYTHONPYCACHEPREFIX=str(temporary / "pycache"),
                           WEB_SMOKE_TIMEOUT="10000")

        def run(name: str, command: list[str | Path], *, cwd: Path = ROOT,
                env: dict[str, str] | None = None, timeout: int = 60) -> tuple[int, str]:
            arguments = [str(item) for item in command]
            log = evidence / f"{name}.log"
            # Ordinary log files avoid the managed sandbox's Node pipe-EPERM
            # behavior and retain the complete worker's failure screen/trace.
            with log.open("w") as output:
                output.write("$ " + shlex.join(arguments) + "\n")
                output.flush()
                result = subprocess.run(arguments, cwd=cwd, env=env or environment,
                                        stdout=output, stderr=subprocess.STDOUT,
                                        timeout=timeout, check=False)
                output.write(f"\nExit status: {result.returncode}\n")
            return result.returncode, log.read_text()

        def green(name: str, result: tuple[int, str], witness: str | None = None) -> None:
            status, output = result
            assert status == 0, f"{name}: expected success\n{output}"
            if witness is not None:
                assert witness in output, f"{name}: successful exit omitted its completion witness\n{output}"

        def red(name: str, result: tuple[int, str], expected: str) -> None:
            status, output = result
            assert status == 1, f"{name}: planted defect did not fail normally\n{output}"
            diagnostic = next((line.strip() for line in output.splitlines() if re.search(expected, line)), None)
            assert diagnostic is not None, f"{name}: failed for an unrelated reason\n{output}"
            report(f"RED {name}: {diagnostic}")

        def smoke(name: str, site: Path, option: str) -> tuple[int, str]:
            return run(name, [node, ROOT / "tests/web_smoke.mjs", site / "vc.mjs", option])

        def copy_site(destination: Path) -> None:
            shutil.copytree(web, destination)
            work = destination.with_name(destination.name + "-work")
            work.mkdir()
            shutil.copytree(Path(f"{web}-work") / "demo", work / "demo")
            shutil.copyfile(Path(f"{web}-work") / "web_program_names.h", work / "web_program_names.h")

        lazy_project = temporary / "lazy"
        lazy_site = lazy_project / "web"
        symbol_site = temporary / "symbol/web"
        copy_site(lazy_site)
        copy_site(symbol_site)
        original_wasm = (web / "vc.wasm").read_bytes()
        original_module = (web / "vc.mjs").read_bytes()
        original_source = (ROOT / "runtime/dos_fs.c").read_bytes()
        assert original_source.count(HOOK) == 1, "DOS-open hook is no longer unique"
        assert len(SYMBOL) == len(WRONG_SYMBOL) and original_wasm.count(SYMBOL) == 1, (
            "main must have exactly one NUL-terminated VC405 lookup symbol"
        )
        report(f"Original main SHA-256: {sha256(original_wasm).hexdigest()}")
        report(f"Lookup mutation: image_vc405 -> image_wrong at file offset 0x{original_wasm.index(SYMBOL):x}.")

        # This directory contains only copied runtime and Makefile sources.
        # Generated C, fonts and tooling are read-only inputs through symlinks.
        shutil.copytree(ROOT / "runtime", lazy_project / "runtime")
        shutil.copyfile(ROOT / "Makefile", lazy_project / "Makefile")
        (lazy_project / "build").mkdir()
        (lazy_project / "build/gen").symlink_to(ROOT / "build/gen", target_is_directory=True)
        for name in ("tools", "third_party", ".venv"):
            (lazy_project / name).symlink_to(ROOT / name, target_is_directory=True)
        shutil.copytree(ROOT / "build/emcache", temporary / "emcache")
        private_source = lazy_project / "runtime/dos_fs.c"
        private_source.write_bytes(original_source.replace(
            HOOK, b"/* Brief 32 mutation: bypass lazy contents on the actual DOS-open path. */"
        ))
        (lazy_project / "mutation.mk").write_text(
            ".PHONY: vc405-mutation-main\n"
            "vc405-mutation-main:\n"
            "\t$(EMCC) $(WEB_FLAGS) -std=gnu11 -Iruntime -I$(WEB_WORK) $(WEB_MAIN_SRC) -o $(WEB_OUT)/vc.mjs\n"
        )
        compiler_environment = dict(environment, EM_CONFIG=str(sdk / ".emscripten"),
                                    EM_CACHE=str(temporary / "emcache"), EMSDK_PYTHON=str(python),
                                    EMCC_CORES="2")
        compile_command = [
            "make", "-f", "Makefile", "-f", "mutation.mk", "vc405-mutation-main",
            f"EMCC={shlex.join([str(python), str(compiler)])}", f"WEB_OUT={lazy_site}",
        ]

        def restore(site: Path) -> None:
            (site / "vc.wasm").write_bytes(original_wasm)
            (site / "vc.mjs").write_bytes(original_module)

        # A main-only compiler job runs alongside the independent symbol
        # mutation; neither writes the other's publication or compiler cache.
        with ThreadPoolExecutor(max_workers=1) as executor:
            compiling = executor.submit(run, "lazy-main-build", compile_command,
                                        cwd=lazy_project, env=compiler_environment, timeout=300)
            try:
                green("baseline lazy files", smoke("baseline-lazy", symbol_site, "--lazy-files-only"),
                      "web lazy-file smoke passed")
                green("baseline VC405", smoke("baseline-vc405", symbol_site, "--vc405-only"),
                      "web VC405 smoke passed")
                report("GREEN baseline: both focused real-translated-VC/MEMFS gates pass.")

                (symbol_site / "vc.wasm").write_bytes(original_wasm.replace(SYMBOL, WRONG_SYMBOL))
                try:
                    red("vc405-module-lookup", smoke("vc405-lookup-red", symbol_site, "--vc405-only"),
                        r"Timed out waiting for Enter starts actual VC 4\.05")
                finally:
                    restore(symbol_site)
                green("restored VC405 main bytes", run("vc405-main-cmp", ["cmp", symbol_site / "vc.wasm", web / "vc.wasm"]))
                green("restored VC405", smoke("vc405-lookup-restored", symbol_site, "--vc405-only"),
                      "web VC405 smoke passed")
                report("GREEN vc405-module-lookup restored: actual cmp and the complete VC405 focused smoke pass.")

                green("private main compile", compiling.result())
                report("Private DOS-open mutant compiled with the current Makefile flags; no side module was rebuilt.")
                try:
                    red("lazy-dos-open-hook", smoke("lazy-hook-red", lazy_site, "--lazy-files-only"),
                        r"Timed out waiting for F3 reaches the lazy DOS-open fetch")
                finally:
                    private_source.write_bytes(original_source)
                    restore(lazy_site)
                green("restored DOS source", run("lazy-source-cmp", ["cmp", private_source, ROOT / "runtime/dos_fs.c"]))
                green("restored lazy main bytes", run("lazy-main-cmp", ["cmp", lazy_site / "vc.wasm", web / "vc.wasm"]))
                green("restored lazy files", smoke("lazy-hook-restored", lazy_site, "--lazy-files-only"),
                      "web lazy-file smoke passed")
                report("GREEN lazy-dos-open-hook restored: source/main cmp and the focused lazy-file smoke pass.")
            finally:
                # Wait for the bounded compiler before removing its own tree.
                compiling.result()
                private_source.write_bytes(original_source)
                restore(lazy_site)
                restore(symbol_site)
        report("GREEN complete: both full-web behavioral mutants rejected, both focused gates restored; live source, main, side modules and compiler cache untouched.")


if __name__ == "__main__":
    main()
