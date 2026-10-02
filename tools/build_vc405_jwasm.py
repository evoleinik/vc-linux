"""Build the isolated offline TASM-compatible JWasm used only by VC 4.05."""

from __future__ import annotations

import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tools/jwasm/jwasm-a7c6e70.tar.gz"
ARCHIVE_SHA256 = "d7b5d9bd24ab65fd448fb4d8082551c8ab750ba81896d85ee7de2003a162a7f6"
PATCH = ROOT / "tools/jwasm/vc405.patch"


def ensure_jwasm(output: Path | None = None) -> Path:
    """Keep the stock, Kermit and MS-DOS assemblers byte-for-byte unchanged."""
    output = (output or ROOT / "build/vc405-toolchain").resolve()
    raw = ARCHIVE.read_bytes()
    if hashlib.sha256(raw).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("the pinned JWasm source archive SHA-256 does not match")
    digest = hashlib.sha256(raw + PATCH.read_bytes() + Path(__file__).read_bytes()
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
            # Only the tool build copy is normalized to make the patch portable.
            # Volkov's source and its CRLF/DOS EOF bytes are never changed.
            for path in (source / "src").rglob("*"):
                if path.suffix in (".c", ".h"):
                    path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))
            subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(PATCH)],
                           cwd=source, check=True, capture_output=True)
            environment = dict(os.environ, SOURCE_DATE_EPOCH="1790899200")
            result = subprocess.run(
                ["make", "-s", "-f", "GccUnix.mak", "-j4",
                 "extra_c_flags=-DNDEBUG -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"],
                cwd=source, env=environment, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            if result.returncode:
                raise RuntimeError("VC 4.05 JWasm build failed:\n" + result.stdout)
            shutil.copy2(source / "build/GccUnixR/jwasm", binary)
        stamp.write_text(digest + "\n")
    return binary


if __name__ == "__main__":
    print(ensure_jwasm())
