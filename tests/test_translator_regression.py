"""Adding a translator input must not change any earlier generated program.

These hashes were captured before Rogue work, then checked with cmp against
the preserved build/rogue-baseline copies. They cover generated source, not
merely an equivalent executable or a selection of translated instructions.
Brief 32 extends the baseline to every earlier program, with actual C copies
saved in build/brief32-prior-translations and checked using cmp at handoff.
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
    "rogue.c": "fb20c57c153b939504cd38762088958e83d4c135baad0eb3064eb36d70bd846f",
    "vz.c": "c9b3d5dbbeb67a4d4d1dc5060fbdfadcbacd0903f73f2b25b168c8a581ee64fc",
    "kermit.c": "56810dbeda2586830b008fc741b99630cb7e9be2c591612e325e8822ea04fb85",
    "command.c": "48129c7f78f5c467a2e9cd67f15427bfcdb1a2f2beed7d3c7a572358ecbffa01",
    "edlin.c": "03639e172ce732aab6001f9d80917f1f3c00c08c3b1f0b18b0ca95262e5dcbf5",
    "debug.c": "eb7a652c0bb4dcbc7e6db79b539f026fa4f9b53350054d6284953677b0aad539",
    "find.c": "0dbb57fd80fe3dc82a5da9efd59ea0e06e41772aa78962ea0d96815d602b8e15",
    "more.c": "d665bf95e4dfd86a0970ec90fa44f1fb61afedbf119d77fe02750a1a63474b19",
    "sort.c": "1a7f5b8c107f279a48e74a2382faf3f83259e569d1bf241e0eb7e8ba8f0f533c",
    "fc.c": "0fdbe45d62be6fc516d7c766e2cee20374ef533e99bb245be8d4f4ada4c66da3",
}


@pytest.mark.parametrize("name", BASELINE)
def test_earlier_generated_c_is_byte_identical(name):
    path = Path(__file__).resolve().parents[1] / "build/gen" / name
    assert sha256(path.read_bytes()).hexdigest() == BASELINE[name], (
        f"{name} changed while adding VC 4.05; "
        "the earlier listing paths must remain byte-identical"
    )
