"""Brief 16: adding the compiled-C input path must not change earlier output.

These hashes were captured before Rogue work, then checked with cmp against
the preserved build/rogue-baseline copies. They cover generated source, not
merely an equivalent executable or a selection of translated instructions.
"""
from hashlib import sha256
from pathlib import Path

import pytest


BASELINE = {
    "vc_com.c": "d4d8f2fe893a384ae7c7c6695e650e862f1a894c1f13572b8c929fc302674817",
    "vc_ovl.c": "f0f51d56d84994b90828127a02d55de241a3a64b898dccc932846b51d5fec9b6",
    "gwbasic.c": "8d8cf3225503fa947a969eb089b820ade134d61938765a78a257da7d43c7fe6f",
    "bootlogo.c": "cbb6ff5d610dfcc97d582aaf17f6cb45aff30a42b5aab4e3038ed712274d0a65",
    "gwbasic_graphics.c": "4aae5dd0c71f389a8a38a02bf8faa8eb35f70999bb52b21b14f63bad3f78b3f6",
}


@pytest.mark.parametrize("name", BASELINE)
def test_earlier_generated_c_is_byte_identical(name):
    path = Path(__file__).resolve().parents[1] / "build/gen" / name
    assert sha256(path.read_bytes()).hexdigest() == BASELINE[name], (
        f"{name} changed while adding Rogue's compiled-C front end; "
        "the earlier listing paths must remain byte-identical"
    )
