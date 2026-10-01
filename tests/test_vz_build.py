"""VZ's assembly compatibility layer may change syntax, never guest behavior."""

import pytest

from tools.build_vz import REFERENCE, ROOT, SOURCE, build, modules, preprocess, verify_image


def test_original_link_order():
    names = modules()
    assert len(names) == len(set(names)) == 29
    assert names[:3] == ["MAIN", "ALIAS", "CHAR"]
    assert names[-5:] == ["WIND", "XSCR", "SWAP", "EMS", "MSG"]


def test_includes_are_case_insensitive():
    assert preprocess("SCRN.ASM", b"\tinclude scrnIBM.asm\r\n") == b"\tinclude SCRNIBM.ASM\n"


def test_function_key_constant_is_not_an_anonymous_forward_label():
    text = b"@F equ 80h\n db @F+1,S@F+1,C@F+2,A@F+3\n"
    assert preprocess("KEYIBM.ASM", text) == (
        b"VZ_FKEY equ 80h\n db VZ_FKEY+1,S@F+1,C@F+2,A@F+3\n")


def test_gdata_separates_macro_arguments():
    assert preprocess("STRING.ASM", b"GDATA vwxapi\tdd,\t0\r\n") == b"GDATA vwxapi,\tdd,\t0\n"
    assert preprocess("MSG.ASM", b"GDATA idword\tlabel\tbyte\r\n") == b"GDATA idword,\tlabel,\tbyte\n"


def test_includes_and_generated_instructions_are_visible():
    assert preprocess("STD.INC", b"\t.xlist\n\t.sall\n") == b"\t.list\n\t.lall\n"


def test_stale_declarations_remain_checked_if_used():
    assert preprocess("MAIN.ASM", b" extrn init_module :near\n extrn init :near\n") == (
        b" externdef init_module :near\n extrn init :near\n")


def test_accumulator_encodings_keep_the_original_immediate_width():
    assert preprocess("MAIN.ASM", b" cmp ax,2\n cmp ax,word ptr x\n sub ax,cx\n") == (
        b" cmp ax,word ptr 2\n cmp ax,word ptr x\n sub ax,word ptr cx\n")


def test_type_expressions_evaluate_before_the_size_cast():
    assert preprocess("FILER.ASM", b" add ax,type _menu ; six bytes\n") == (
        b" add ax,word ptr (+type _menu) ; six bytes\n")


def test_final_even_retains_the_shipped_nop():
    assert preprocess("MSG.ASM", b" even\n") == b" db (($ - mg_remove) and 1) dup (90h)"


def test_build_cannot_overwrite_vendored_sources():
    for output in (SOURCE, SOURCE.parent, REFERENCE.parent, ROOT):
        with pytest.raises(ValueError, match="separate from the vendored sources"):
            build(output)


def test_identity_check_rejects_code_settings_and_size_changes(tmp_path):
    data = REFERENCE.read_bytes()
    path = tmp_path / "bad.com"
    for offset in (0, 0x26, len(data) - 1):
        mutated = bytearray(data)
        mutated[offset] ^= 1
        path.write_bytes(mutated)
        with pytest.raises(ValueError, match=f"offset 0x{offset:x} "):
            verify_image(path)
    path.write_bytes(data[:-1])
    with pytest.raises(ValueError, match="built 55855 bytes, shipped 55856 bytes"):
        verify_image(path)


def test_vendor_sources_are_untouched():
    source = ROOT / "third_party/vzeditor/SRC"
    assert b"@F\t\tequ\t10000000b" in (source / "KEYIBM.ASM").read_bytes()
    assert b"GDATA vwxapi\tdd,\t0" in (source / "STRING.ASM").read_bytes()
    assert b"include\tscrnIBM.asm" in (source / "SCRN.ASM").read_bytes()


def test_rebuild_matches_shipped_and_preserves_vendor(tmp_path):
    originals = {p: p.read_bytes() for p in SOURCE.iterdir() if p.is_file()}
    output = tmp_path / "first"
    build(output)
    assert (output / "VZ.COM").read_bytes() == REFERENCE.read_bytes()
    assert all(path.read_bytes() == data for path, data in originals.items())
    assert "0000:0100" in (output / "VZ.MAP").read_text()
    assert len(list(output.glob("*.lst"))) == 29
    assert all((output / f"{module}.lst").stat().st_size > 1000 for module in modules())


def test_images_maps_and_expanded_listings_are_reproducible(tmp_path):
    outputs = [tmp_path / "first", tmp_path / "second"]
    for output in outputs:
        build(output)
    names = ["VZ.COM", "VZ.MAP"] + [f"{module}.lst" for module in modules()]
    for name in names:
        assert (outputs[0] / name).read_bytes() == (outputs[1] / name).read_bytes(), name
