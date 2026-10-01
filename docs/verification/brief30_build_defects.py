"""Opt-in, process-local pytest mutations; never edit a vendored/workspace file.

PYTHONPATH=docs/verification:. .venv/bin/python -m pytest \
  -p brief30_build_defects --msdos-defect=identity \
  tests/test_msdos_build.py::test_per_program_identity_report_is_measured_and_pinned
"""

from pathlib import Path


def pytest_addoption(parser):
    parser.addoption("--msdos-defect", default="")


def pytest_configure(config):
    defect = config.getoption("--msdos-defect")
    if not defect:
        return
    from tools import build_msdos as builder

    if defect == "provenance":
        builder.verify_sources = lambda source=builder.SOURCE: {}
    elif defect == "checksum":
        original = builder.verify_sources
        builder.verify_sources = lambda source=builder.SOURCE: original(builder.SOURCE)
        builder.verify_staged_sources = lambda *args: None
    elif defect == "reproducibility":
        original = builder.build

        def build(output, *args, **kwargs):
            original(output, *args, **kwargs)
            path = Path(output) / "rdata.obj"
            path.write_bytes(path.read_bytes() + Path(output).name.encode())

        builder.build = build
    elif defect == "identity":
        original = builder.compare

        def compare(*args, **kwargs):
            report = original(*args, **kwargs)
            if report["first_difference"] is not None:
                report["first_difference"] += 1
            return report

        builder.compare = compare
    elif defect == "comlink":
        values = list(builder.PROGRAMS["COMMAND.COM"])
        values[1], values[2] = values[2], values[1]
        builder.PROGRAMS["COMMAND.COM"] = tuple(values)
    elif defect == "include-alias":
        original = builder.shutil.copyfile

        def copyfile(source, destination, *args, **kwargs):
            if Path(source).name == "DOSSYM_v211.ASM" and Path(destination).name == "DOSSYM.ASM":
                source = Path(source).with_name("DOSSYM.ASM")
            return original(source, destination, *args, **kwargs)

        builder.shutil.copyfile = copyfile
    elif defect == "listing":
        from tools.build_kermit import repair_data_listing
        builder.repair_data_listing = repair_data_listing
    elif defect == "mz-origin":
        original = builder.exe2com
        builder.exe2com = lambda raw: bytes(0x100) + original(raw)
    elif defect == "sort-allocation":
        builder.sort_exemod = lambda raw: raw
    elif defect == "sort-header-guard":
        builder.sort_exemod = lambda raw: raw[:12] + b"\x01\x00" + raw[14:]
    elif defect == "sort-payload":
        original = builder.sort_exemod

        def sort_exemod(raw):
            result = bytearray(original(raw))
            header_size = int.from_bytes(result[8:10], "little") * 16
            result[header_size] ^= 1
            return bytes(result)

        builder.sort_exemod = sort_exemod
    elif defect == "legacy-tool":
        from tools.build_kermit_jwasm import ensure_jwasm
        builder.ensure_jwasm = ensure_jwasm
    elif defect == "options":
        original = builder.subprocess.run

        def run(command, *args, **kwargs):
            command = [arg for arg in command if "-Fi" not in str(arg)]
            return original(command, *args, **kwargs)

        builder.subprocess.run = run
    else:
        raise ValueError(f"unknown MS-DOS defect: {defect}")
