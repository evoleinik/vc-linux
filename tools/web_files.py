"""Publish immutable lazy DOS data named by embed.py's generated manifest.

Usage: .venv/bin/python tools/web_files.py FILES.web.json PUBLISH_DIRECTORY WORK_DIRECTORY
Native builds only generate the manifest. This publisher is browser-only and
uses exactly the source bytes whose size/checksum/reference are in the main.
"""
from hashlib import sha256
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.web_modules import update


GENERATED_NAME = re.compile(r"file\.[0-9a-f]{12}\.bin")


def publish(manifest: Path, destination: Path, work: Path) -> tuple[int, int]:
    document = json.loads(manifest.read_text(encoding="utf-8"))
    if document.get("format") != 1 or not isinstance(document.get("files"), list):
        raise ValueError("Unsupported lazy-file manifest")
    files = {}
    for entry in document["files"]:
        data = Path(entry["source"]).read_bytes()
        digest = sha256(data).hexdigest()
        name = entry["asset"]
        if (len(data) != entry["size"] or digest != entry["sha256"] or
                name != f"file.{digest[:12]}.bin"):
            raise ValueError(f"Lazy input changed since the main was generated: {entry['name']}")
        if name in files and files[name] != data:
            raise ValueError(f"Lazy-file hash prefix collision: {name}")
        files[name] = data
    # Validate every input before changing the published site.
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        update(destination / name, data)
    for path in sorted(destination.iterdir()):
        if path.name in files or not GENERATED_NAME.fullmatch(path.name):
            continue
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"Refusing to retire non-regular lazy file: {path}")
        data = path.read_bytes()
        retired = work / "retired-files" / f"{path.name}.{sha256(data).hexdigest()}"
        retired.parent.mkdir(parents=True, exist_ok=True)
        if retired.exists():
            if retired.read_bytes() != data:
                raise ValueError(f"Refusing to overwrite different retired lazy file: {retired}")
            path.unlink()  # the identical recoverable copy already exists
        else:
            path.rename(retired)
        print(f"web files: retired {path} to {retired}")
    return len(files), sum(map(len, files.values()))


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    count, size = publish(*(Path(argument) for argument in sys.argv[1:]))
    print(f"web files: published {count} immutable file contents ({size:,} raw bytes)")


if __name__ == "__main__":
    main()
