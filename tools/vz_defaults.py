"""Build the installed VZ definition without editing the upstream copy."""
from pathlib import Path
import sys


SOURCE = Path(__file__).resolve().parents[1] / "third_party/vzeditor/VZ-IBM/VZIBM.DEF"
BACKUP_OFF = b"\r\nEb-\t\t\t;make backup\r\n"
BACKUP_ON = b"\r\nEb+\t\t\t;make backup\r\n"


def installed_definition() -> bytes:
    original = SOURCE.read_bytes()
    if original.count(BACKUP_OFF) != 1:
        raise ValueError("expected exactly one upstream VZ backup option")
    return original.replace(BACKUP_OFF, BACKUP_ON, 1)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: vz_defaults.py OUTPUT")
    destination = Path(sys.argv[1])
    if destination.resolve().is_relative_to(SOURCE.parent.parent.resolve()):
        raise SystemExit("generated VZ defaults must be separate from the vendor files")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(installed_definition())
