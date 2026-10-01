"""Compile all production-emitted instruction bodies into one Unicorn test library.

Small translation units keep compiler memory modest. Content-addressed objects
in the temporary directory make `pytest -k sbb` bug checks practical.
"""

from __future__ import annotations

from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
import ctypes
from dataclasses import dataclass, replace
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from capstone import x86_const as x86
import unicorn

from translator.emit import emit_instruction_function, emit_prelude
from translator.image import load_image
from translator.listing import parse_listing


ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(__file__).resolve().parent
STATES_PER_INSTRUCTION = 32
REGISTERS = {
    name: index
    for index, name in enumerate(
        ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "es", "cs", "ss", "ds", "ip")
    )
}
BYTE_REGISTERS = {
    "al": (0, 1), "ah": (0, 2), "bl": (1, 1), "bh": (1, 2),
    "cl": (2, 1), "ch": (2, 2), "dl": (3, 1), "dh": (3, 2),
}
STRING_OPERATIONS = {
    "movsb", "movsw", "cmpsb", "cmpsw", "stosb", "stosw",
    "lodsb", "lodsw", "scasb", "scasw", "insb", "insw", "outsb", "outsw",
}


@dataclass(frozen=True)
class InstructionCase:
    index: int
    image_name: str
    record: object
    relocations: tuple[int, ...]

    @property
    def name(self) -> str:
        return self.record.insn.mnemonic.split()[-1]

    @property
    def symbol(self) -> str:
        return f"ops_instruction_{self.index:05d}"


@lru_cache(maxsize=1)
def load_cases() -> tuple[InstructionCase, ...]:
    from translator.layout import build_layout
    from translator.linked import build_linked_layout
    from translator.nasm import build_nasm_layout, parse_nasm_listing
    from translator.supplement import build_gwbasic_graphics_layout
    from translator.compiled import build_compiled_layout
    from tools.build_gwbasic import modules
    from tools.build_vz import modules as vz_modules
    from tools.build_kermit import modules as kermit_modules

    # Stock listings omit assembler-generated prologue/epilogue boundaries.
    # Always ask make to check dependencies so direct pytest runs cannot test
    # stale images/listings after an assembly-source or translator change.
    generated = subprocess.run(["make", "gen"], cwd=ROOT, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert generated.returncode == 0, generated.stdout
    result = []
    seen = set()
    for name in ("VC.COM", "VC.OVL", "GWBASIC.EXE", "LOGO.COM", "ROGUE.EXE", "VZ.COM", "KERMIT.EXE"):
        if name == "ROGUE.EXE":
            directory = ROOT / "build" / "rogue"
            loaded = load_image(directory / name)
            layout = build_compiled_layout(loaded, directory / "ROGUE.MAP")
        elif name == "GWBASIC.EXE":
            directory = ROOT / "build" / "gwbasic"
            loaded = load_image(directory / name)
            layout = build_linked_layout(loaded, [directory / (module + ".lst") for module in modules()],
                                         directory / "GWBASIC.MAP")
            extra = build_gwbasic_graphics_layout(layout)
            layout.instructions = sorted(layout.instructions + extra.instructions, key=lambda record: record.off)
        elif name == "LOGO.COM":
            directory = ROOT / "build" / "bootlogo"
            loaded = load_image(directory / name)
            layout = build_nasm_layout(loaded, parse_nasm_listing(directory / "LOGO.lst"))
        elif name == "VZ.COM":
            directory = ROOT / "build" / "vz"
            loaded = load_image(directory / name)
            layout = build_linked_layout(loaded, [directory / (module + ".lst") for module in vz_modules()],
                                         directory / "VZ.MAP")
        elif name == "KERMIT.EXE":
            directory = ROOT / "build" / "kermit"
            loaded = load_image(directory / name)
            layout = build_linked_layout(loaded, [directory / (module + ".lst") for module in kermit_modules()],
                                         directory / "KERMIT.MAP")
        else:
            loaded = load_image(ROOT / "build" / name)
            layout = build_layout(loaded, parse_listing(ROOT / "build" / "gen" / f"{name}.lst"))
        relocations = tuple(loaded.relocations)
        forms = []
        for record in layout.instructions:
            if record.variants:
                # Test both permitted byte templates through the production
                # guarded dispatcher, not merely their inner semantics.
                forms.extend(replace(variant, variants=record.variants) for variant in record.variants)
            else:
                forms.append(record)
        for record in forms:
            raw = bytes(record.insn.bytes)
            local_relocations = tuple(at - record.off for at in relocations
                                      if record.off <= at < record.off + len(raw))
            # Preserve distinct relocation meanings even if bytes/offset match.
            key = (raw, record.off, local_relocations, getattr(record, "mutable_offsets", ()),
                   tuple(bytes(variant.insn.bytes) for variant in record.variants))
            if key in seen:
                continue
            seen.add(key)
            result.append(InstructionCase(len(result), name, record, relocations))
    assert result, "The listings must contain translated instructions"
    return tuple(result)


def group_cases(cases: tuple[InstructionCase, ...]) -> dict[str, tuple[InstructionCase, ...]]:
    groups = defaultdict(list)
    for case in cases:
        groups[case.name].append(case)
    return {name: tuple(groups[name]) for name in sorted(groups)}


def _undefined_flags(insn) -> int:
    result = 0
    for name, bit in (("CF", 0), ("PF", 2), ("AF", 4), ("ZF", 6), ("SF", 7), ("OF", 11)):
        if insn.eflags & getattr(x86, "X86_EFLAGS_UNDEFINED_" + name):
            result |= 1 << bit
    return result


def _metadata(case: InstructionCase) -> str:
    insn = case.record.insn
    name = case.name
    raw = bytes(insn.bytes)
    operands = list(insn.operands)
    memories = []
    memory_positions = {}
    for index, operand in enumerate(operands):
        if operand.type != x86.X86_OP_MEM:
            continue
        memory_positions[index] = len(memories)
        base = insn.reg_name(operand.mem.base) if operand.mem.base else None
        second = insn.reg_name(operand.mem.index) if operand.mem.index else None
        segment = (insn.reg_name(operand.mem.segment) if operand.mem.segment else
                   "ss" if base in ("bp", "sp") or second in ("bp", "sp") else "ds")
        if operand.mem.scale not in (0, 1):
            raise AssertionError(f"16-bit oracle does not model scale {operand.mem.scale}: {insn}")
        memories.append("{%d, %d, %d, %d, %d, %d}" % (
            REGISTERS[base] if base else -1,
            REGISTERS[second] if second else -1,
            REGISTERS[segment], operand.size, bool(operand.access & 2), operand.mem.disp,
        ))
    assert len(memories) <= 3
    flag_kind = {
        "shl": "OPS_SHIFT_LEFT", "sal": "OPS_SHIFT_LEFT",
        "shr": "OPS_SHIFT_RIGHT", "sar": "OPS_SHIFT_ARITH",
        "rol": "OPS_ROTATE", "ror": "OPS_ROTATE",
        "rcl": "OPS_ROTATE_CARRY", "rcr": "OPS_ROTATE_CARRY",
        "div": "OPS_DIV", "idiv": "OPS_IDIV",
    }.get(name, "OPS_NORMAL")
    width = operands[0].size * 8 if operands else 0
    count_from_cl = 0
    immediate_count = 0
    if flag_kind not in ("OPS_NORMAL", "OPS_DIV", "OPS_IDIV"):
        assert len(operands) == 2, f"unexpected shift/rotate operands: {insn}"
        if operands[1].type == x86.X86_OP_REG:
            assert insn.reg_name(operands[1].reg) == "cl"
            count_from_cl = 1
        else:
            immediate_count = operands[1].imm & 0xff
    special = []
    vector = 0
    if name in ("int", "int3", "int1"):
        special.append("OPS_INT")
        vector = operands[0].imm if name == "int" else 3 if "3" in name else 1
    if name == "into":
        special.append("OPS_INTO")
    if name in STRING_OPERATIONS and any(prefix in (0xf2, 0xf3) for prefix in insn.prefix):
        special.append("OPS_REP")
    stack_write_words = {
        "push": 1, "pushf": 1, "pushaw": 8, "pusha": 8,
        "call": 1, "lcall": 2, "int": 3, "int3": 3, "int1": 3,
        "into": 3, "div": 3, "idiv": 3,
    }.get(name, 0)
    divisor_register, divisor_part, divisor_memory = -1, 0, -1
    if name in ("div", "idiv"):
        if operands[0].type == x86.X86_OP_MEM:
            divisor_memory = memory_positions[0]
        else:
            register = insn.reg_name(operands[0].reg)
            if register in BYTE_REGISTERS:
                divisor_register, divisor_part = BYTE_REGISTERS[register]
            else:
                divisor_register = REGISTERS[register]
    local_relocations = [at - case.record.off for at in case.relocations
                         if case.record.off <= at < case.record.off + len(raw)]
    mutable = getattr(case.record, "mutable_offsets", ())
    assert len(raw) <= 16 and len(local_relocations) <= 8
    description = f"{case.image_name}:0x{case.record.off:05x} {insn.mnemonic} {insn.op_str}".strip()
    return (
        "{.run=%s, .description=%s, .offset=%d, .bytes={%s}, .length=%d,\n"
        " .relocation_count=%d, .relocations={%s}, .undefined_flags=0x%04x,\n"
        " .mutable_count=%d, .mutable_offsets={%s},\n"
        " .flags_kind=%s, .width=%d, .count_from_cl=%d, .immediate_count=%d,\n"
        " .special=%s, .vector=%d, .memory_count=%d, .stack_write_words=%d, .operands={%s},\n"
        " .divisor_register=%d, .divisor_part=%d, .divisor_memory=%d}"
    ) % (
        case.symbol, json.dumps(description), case.record.off,
        ",".join(f"0x{byte:02x}" for byte in raw), len(raw),
        len(local_relocations), ",".join(map(str, local_relocations)) or "0",
        _undefined_flags(insn), len(mutable), ",".join(map(str, mutable)) or "0",
        flag_kind, width, count_from_cl, immediate_count,
        " | ".join(special) or "0", vector, len(memories), stack_write_words, ",".join(memories) or "{0}",
        divisor_register, divisor_part, divisor_memory,
    )


def _run(command: list[str]) -> None:
    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    if completed.returncode:
        raise AssertionError("C test build failed:\n" + " ".join(command) + "\n" + completed.stdout)


def build_library(cases: tuple[InstructionCase, ...]) -> ctypes.CDLL:
    """Compile every emitted instruction and return the one linked test library."""
    cache = Path(tempfile.gettempdir()) / f"vc-translator-tests-{os.getuid()}"
    cache.mkdir(parents=True, exist_ok=True)
    unicorn_package = Path(unicorn.__file__).resolve().parent
    unicorn_library = unicorn_package / "lib" / "libunicorn.so.2"
    assert unicorn_library.is_file(), f"missing bundled Unicorn C library: {unicorn_library}"
    flags = ["gcc", "-O1", "-Wall", "-Wno-unused-label", "-fPIC", "-DCPU_TRACE_WRITES",
             "-I" + str(ROOT / "runtime"), "-I" + str(SUPPORT),
             "-I" + str(unicorn_package / "include")]
    dependencies = hashlib.sha256()
    for path in (ROOT / "runtime" / "cpu.c", ROOT / "runtime" / "cpu.h",
                 ROOT / "runtime" / "image.h", SUPPORT / "ops_harness.c",
                 SUPPORT / "ops_harness.h"):
        dependencies.update(path.read_bytes())
    dependencies.update(unicorn.__version__.encode())
    dependencies.update("\0".join(flags).encode())
    dependencies.update(subprocess.check_output(["gcc", "--version"]))
    dependency_digest = dependencies.digest()
    prelude = emit_prelude()
    sources = []
    for group in group_cases(cases).values():
        for beginning in range(0, len(group), 1024):
            subset = group[beginning:beginning + 1024]
            source = prelude + "\n" + "\n".join(
                emit_instruction_function(case.record, case.relocations, case.symbol)
                for case in subset
            )
            sources.append(source)
    central = "#include " + json.dumps(str(SUPPORT / "ops_harness.c")) + "\n"
    central += "\n".join(f"extern int {case.symbol}(uint16_t);" for case in cases)
    central += "\nconst OpsInstruction ops_instructions[] = {\n"
    central += ",\n".join(_metadata(case) for case in cases)
    central += "\n};\nconst unsigned ops_instruction_count = sizeof ops_instructions / sizeof ops_instructions[0];\n"
    sources.extend((central, '#include ' + json.dumps(str(ROOT / "runtime" / "cpu.c")) + "\n"))

    def compile_source(source: str) -> Path:
        digest = hashlib.sha256(dependency_digest + source.encode()).hexdigest()
        stem = cache / ("ops-" + digest)
        object_file = stem.with_suffix(".o")
        if not object_file.exists():
            c_file = stem.with_suffix(".c")
            c_file.write_text(source)
            temporary = Path(str(object_file) + f".{os.getpid()}.tmp")
            _run([*flags, "-c", str(c_file), "-o", str(temporary)])
            temporary.replace(object_file)
        return object_file

    with ThreadPoolExecutor(max_workers=2) as executor:
        objects = list(executor.map(compile_source, sources))
    library_digest = hashlib.sha256("\n".join(map(str, objects)).encode()).hexdigest()
    library_path = cache / ("ops-library-" + library_digest + ".so")
    if not library_path.exists():
        temporary = Path(str(library_path) + f".{os.getpid()}.tmp")
        _run(["gcc", "-shared", *(str(path) for path in objects), str(unicorn_library),
              "-Wl,-rpath," + str(unicorn_library.parent), "-o", str(temporary)])
        temporary.replace(library_path)
    result = ctypes.CDLL(str(library_path))
    result.ops_check_instruction.argtypes = (ctypes.c_uint, ctypes.c_uint)
    result.ops_check_instruction.restype = ctypes.c_char_p
    result.ops_checked_states.argtypes = ()
    result.ops_checked_states.restype = ctypes.c_uint64
    result.ops_checked_divide_errors.argtypes = ()
    result.ops_checked_divide_errors.restype = ctypes.c_uint64
    result.ops_resampled_states.argtypes = ()
    result.ops_resampled_states.restype = ctypes.c_uint64
    return result
