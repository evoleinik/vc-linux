"""Build MS-DOS's isolated legacy-MASM tool fixes from pinned offline JWasm."""

from __future__ import annotations

import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

from tools.build_kermit_jwasm import ARCHIVE, ARCHIVE_SHA256, ROOT


PATCHES = (ROOT / "tools/jwasm/kermit.patch", ROOT / "tools/jwasm/msdos.patch")


def ensure_jwasm(output: Path | None = None) -> Path:
    """Neither VC's stock assembler nor Kermit's private assembler changes."""
    output = (output or ROOT / "build/msdos-toolchain").resolve()
    raw = ARCHIVE.read_bytes()
    if hashlib.sha256(raw).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("the pinned JWasm source archive SHA-256 does not match")
    digest = hashlib.sha256(raw + b"".join(p.read_bytes() for p in PATCHES) + Path(__file__).read_bytes()
                            + subprocess.check_output(["gcc", "--version"])).hexdigest()
    output.mkdir(parents=True, exist_ok=True)
    binary, stamp = output / "jwasm", output / "inputs.sha256"
    with (output / "build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if binary.is_file() and stamp.is_file() and stamp.read_text().strip() == digest:
            return binary
        with tempfile.TemporaryDirectory(prefix="source-", dir=output) as temporary:
            source = Path(temporary)
            with tarfile.open(ARCHIVE) as archive:
                archive.extractall(source, filter="data")
            for path in (source / "src").rglob("*"):
                if path.suffix in (".c", ".h"):
                    path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))
            for patch in PATCHES:
                subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
                               cwd=source, check=True)
            environment = dict(os.environ, SOURCE_DATE_EPOCH="1790899200")
            subprocess.run(["make", "-s", "-f", "GccUnix.mak", "-j4",
                            "extra_c_flags=-DNDEBUG -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"],
                           cwd=source, env=environment, check=True)
            shutil.copy2(source / "build/GccUnixR/jwasm", binary)
        stamp.write_text(digest + "\n")
    return binary
