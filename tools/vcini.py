"""Read or change fields of a VC.INI, keeping VC's checksum valid.

    python tools/vcini.py FILE                  print the fields tests care about
    python tools/vcini.py FILE ConvCase=0 ...   set main-block byte fields

Field offsets come from the JWasm listing (build/gen/VC.OVL.lst or
build/VC.OVL.lst), so nothing here hard-codes VC's data layout.

Layout, from VC.ASM Init57: 'VVV', the left panel block (WCBini bytes), the
right panel block, the main block (CmnIni bytes), then a 16-bit sum of every
byte before it.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def listing() -> str:
    for p in (ROOT / "build/gen/VC.OVL.lst", ROOT / "build/VC.OVL.lst"):
        if p.exists():
            return p.read_text(encoding="latin-1")
    raise SystemExit("no VC.OVL listing: run make images")


def symbols() -> dict[str, int]:
    """Byte fields and numbers from the listing's symbol tables."""
    out: dict[str, int] = {}
    text = listing()
    # struct fields: "  ConvCase . . . .   12A   Byte"
    for m in re.finditer(r"^\s{0,2}(\w+) +\.[ .]*\s+([0-9A-F]+)\s+Byte", text, re.M):
        out.setdefault(m.group(1), int(m.group(2), 16))
    # constants: "WCBini . . . .   Number   12Eh"
    for m in re.finditer(r"^(\w+) +\.[ .]*\s+Number\s+([0-9A-F]+)h", text, re.M):
        out.setdefault(m.group(1), int(m.group(2), 16))
    return out


class VcIni:
    def __init__(self, data: bytes):
        s = symbols()
        self.wcb, self.cmn = s["WCBini"], s["CmnIni"]
        self.sym = s
        self.data = bytearray(data)
        expected = 3 + 2 * self.wcb + self.cmn + 2
        if len(self.data) != expected:
            raise ValueError(f"VC.INI is {len(self.data)} bytes, VC expects {expected}")
        if self.data[:3] != b"VVV":
            raise ValueError("VC.INI does not start with VVV")

    @property
    def checksum_ok(self) -> bool:
        n = len(self.data) - 2
        return sum(self.data[:n]) & 0xFFFF == self.data[n] | self.data[n + 1] << 8

    def fix_checksum(self) -> None:
        n = len(self.data) - 2
        total = sum(self.data[:n]) & 0xFFFF
        self.data[n], self.data[n + 1] = total & 0xFF, total >> 8

    def main_offset(self, field: str) -> int:
        return 3 + 2 * self.wcb + self.sym[field]

    def panel_offset(self, panel: int, field: str) -> int:
        if panel not in (0, 1):
            raise ValueError("panel must be 0 (left) or 1 (right)")
        return 3 + panel * self.wcb + self.sym[field]

    def main(self, field: str) -> int:
        return self.data[self.main_offset(field)]

    def panel(self, panel: int, field: str) -> int:
        return self.data[self.panel_offset(panel, field)]

    def set_main(self, field: str, value: int) -> None:
        self.data[self.main_offset(field)] = value
        self.fix_checksum()

    def set_panel(self, panel: int, field: str, value: int) -> None:
        self.data[self.panel_offset(panel, field)] = value
        self.fix_checksum()

    def panel_path(self, panel: int) -> str:
        start = self.panel_offset(panel, "WinShortPath")
        value = bytes(self.data[start:start + self.sym["LenPath"]])
        return value.split(b"\0", 1)[0].decode("cp866")

    def set_panel_path(self, panel: int, path: str) -> None:
        """Set the panel's saved DOS path; both offset and capacity are listed."""
        start = self.panel_offset(panel, "WinShortPath")
        capacity = self.sym["LenPath"]
        value = path.encode("cp866")
        if b"\0" in value or len(value) >= capacity:
            raise ValueError("panel path must fit LenPath including its NUL terminator")
        self.data[start:start + capacity] = value.ljust(capacity, b"\0")
        self.fix_checksum()


def main() -> None:
    path = Path(sys.argv[1])
    ini = VcIni(path.read_bytes())
    for arg in sys.argv[2:]:
        field, value = arg.split("=", 1)
        ini.set_main(field, int(value, 0))
    if sys.argv[2:]:
        path.write_bytes(ini.data)
    print(f"checksum_ok={ini.checksum_ok} ConvCase={ini.main('ConvCase')} Active={ini.main('Active')} "
          f"visible={ini.panel(0, 'Visible')},{ini.panel(1, 'Visible')} "
          f"long_names={ini.panel(0, 'LongNames')},{ini.panel(1, 'LongNames')}")


if __name__ == "__main__":
    main()
