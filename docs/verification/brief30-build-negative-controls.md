# Brief 30 build negative-control execution log

Captured 2026-10-02. Every invocation below exited 1; none mutated the shared
production files. The opt-in pytest plugin applies each defect in its own process.

## provenance

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=provenance \
  tests/test_msdos_build.py::test_vendor_manifest_preserves_every_upstream_byte_and_license --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
________ test_vendor_manifest_preserves_every_upstream_byte_and_license ________
tests/test_msdos_build.py:33: in test_vendor_manifest_preserves_every_upstream_byte_and_license
    assert len(manifest) == 57
E   assert 0 == 57
E    +  where 0 = len({})
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_vendor_manifest_preserves_every_upstream_byte_and_license
1 failed in 0.03s
```

## reproducibility

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=reproducibility \
  tests/test_msdos_build.py::test_images_maps_objects_listings_and_comparison_are_reproducible --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
______ test_images_maps_objects_listings_and_comparison_are_reproducible _______
tests/test_msdos_build.py:65: in test_images_maps_objects_listings_and_comparison_are_reproducible
    assert (builds[0] / name).read_bytes() == (builds[1] / name).read_bytes(), name
E   AssertionError: rdata.obj
E   assert b'\x80\x0b\x0...x00\x00tfirst' == b'\x80\x0b\x0...00\x00tsecond'
E     
E     At index 2414 diff: b'f' != b's'
E     Use -v to get more diff
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_images_maps_objects_listings_and_comparison_are_reproducible
1 failed in 1.72s
```

## identity

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=identity \
  tests/test_msdos_build.py::test_per_program_identity_report_is_measured_and_pinned --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
___________ test_per_program_identity_report_is_measured_and_pinned ____________
tests/test_msdos_build.py:73: in test_per_program_identity_report_is_measured_and_pinned
    assert measured == expected
E   AssertionError: assert [{'program': ...52, ...}, ...] == [{'program': ...52, ...}, ...]
E     
E     At index 0 diff: {'program': 'COMMAND.COM', 'identical': False, 'first_difference': 2, 'built_size': 17952, 'shipped_size': 15480, 'built_sha256': '46c053a9f51ad18518dfefe0f85c5b6d8e6015998a83f1412764e0cf15210a35', 'shipped_sha256': '4cc71b3692b894eef9a7c8b3ac0fc63a71bdfa08175089562fdc4c7c5b038e8a', 'built_byte': 205, 'shipped_byte': 109} != {'program': 'COMMAND.COM', 'identical': False, 'first_difference': 1, 'built_size': 17952, 'shipped_size': 15480, 'built_sha256': '46c053a9f51ad18518dfefe0f85c5b6d8e6015998a83f1412764e0cf15210a35', 'shipped_sha256': '4cc71b3692b894e...
E     
E     ...Full output truncated (2 lines hidden), use '-vv' to show
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_per_program_identity_report_is_measured_and_pinned
1 failed in 1.71s
```

## comlink

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=comlink \
  tests/test_msdos_build.py::test_original_command_comlink_order_and_all_module_contributions --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_______ test_original_command_comlink_order_and_all_module_contributions _______
tests/test_msdos_build.py:54: in test_original_command_comlink_order_and_all_module_contributions
    assert re.findall(r"\b[a-z][a-z0-9]*\b", original) == builder.modules("COMMAND.COM")
E   AssertionError: assert ['command', '... 'tcode', ...] == ['command', '... 'tcode', ...]
E     
E     At index 1 diff: 'rucode' != 'rdata'
E     Use -v to get more diff
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_original_command_comlink_order_and_all_module_contributions
1 failed in 1.71s
```

## include-alias

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=include-alias \
  tests/test_msdos_build.py::test_each_assembler_input_is_unedited_with_versioned_include_aliases --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_____ test_each_assembler_input_is_unedited_with_versioned_include_aliases _____
tests/test_msdos_build.py:104: in test_each_assembler_input_is_unedited_with_versioned_include_aliases
    builder.build(tmp_path / "checked")
tools/build_msdos.py:167: in build
    verify_staged_sources(stage, expected, program)
tools/build_msdos.py:99: in verify_staged_sources
    raise ValueError(f"assembler input was edited: {name} ({alias})")
E   ValueError: assembler input was edited: source/DOSSYM.ASM (DOSSYM.ASM)
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_each_assembler_input_is_unedited_with_versioned_include_aliases
1 failed in 0.05s
```

## listing

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=listing \
  tests/test_msdos_build.py::test_listing_data_repair_preserves_instruction_fixup_markers --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_________ test_listing_data_repair_preserves_instruction_fixup_markers _________
tests/test_msdos_build.py:129: in test_listing_data_repair_preserves_instruction_fixup_markers
    assert re.search(rb"BA\s+[0-9A-F]+[os].*MOV\s+DX,OFFSET BADVER", before)
E   AssertionError: assert None
E    +  where None = <function search at 0x711339383ba0>(b'BA\\s+[0-9A-F]+[os].*MOV\\s+DX,OFFSET BADVER', b'JWasm v2.21, Oct  2 2026\nmore.asm\n                              C ; DOSMAC.ASM predates MASM\'s INVOKE keyword. Do...le . . . . . .        Number             6h \n\nmore.asm: 152 lines, 3 passes, elapsed omitted, 0 warnings, 0 errors\n')
E    +    where <function search at 0x711339383ba0> = re.search
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_listing_data_repair_preserves_instruction_fixup_markers
1 failed in 1.74s
```

## mz-origin

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=mz-origin \
  tests/test_msdos_build.py::test_command_mz_conversion_preserves_third_group_and_rejects_fixups --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_____ test_command_mz_conversion_preserves_third_group_and_rejects_fixups ______
tests/test_msdos_build.py:142: in test_command_mz_conversion_preserves_third_group_and_rejects_fixups
    assert com[exec_address - 0x100:exec_address - 0x100 + 7] == bytes.fromhex("0e1fb80033cd21")
E   AssertionError: assert b'\x00\x00\x0...0\x00\x00\x00' == b'\x0e\x1f\xb8\x003\xcd!'
E     
E     At index 0 diff: b'\x00' != b'\x0e'
E     Use -v to get more diff
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_command_mz_conversion_preserves_third_group_and_rejects_fixups
1 failed in 1.70s
```

## checksum

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=checksum \
  tests/test_msdos_build.py::test_changed_source_cannot_reuse_an_old_success --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_______________ test_changed_source_cannot_reuse_an_old_success ________________
tests/test_msdos_build.py:114: in test_changed_source_cannot_reuse_an_old_success
    with pytest.raises(ValueError, match="differs from the unedited upstream: source/COMMAND.ASM"):
         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
E   Failed: DID NOT RAISE ValueError
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_changed_source_cannot_reuse_an_old_success
1 failed in 2.36s
```

## legacy-tool

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=legacy-tool \
  tests/test_msdos_build.py::test_legacy_masm_tool_fixes_are_required_without_source_edits --tb=short --show-capture=no
```

```text
FFFFFF                                                                   [100%]
=================================== FAILURES ===================================
_ test_legacy_masm_tool_fixes_are_required_without_source_edits[escaped_directives] _
tests/test_msdos_build.py:178: in test_legacy_masm_tool_fixes_are_required_without_source_edits
    assert result.returncode == 0, result.stderr.decode(errors="replace")
E   AssertionError: /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar0/escaped_directives.asm(14) : Fatal error A1162: Unmatched macro nesting
E     
E   assert 1 == 0
E    +  where 1 = CompletedProcess(args=['/home/eo/src/vc-linux-wt/command/build/kermit-toolchain/jwasm', '-q', '-Zm', '-bin', '-Fo=new....o/pytest-48/test_legacy_masm_tool_fixes_ar0/escaped_directives.asm(14) : Fatal error A1162: Unmatched macro nesting\n').returncode
_ test_legacy_masm_tool_fixes_are_required_without_source_edits[percent_concat] _
tests/test_msdos_build.py:178: in test_legacy_masm_tool_fixes_are_required_without_source_edits
    assert result.returncode == 0, result.stderr.decode(errors="replace")
E   AssertionError: /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar1/percent_concat.asm(8) : Error A2209: Syntax error: code
E      maker(1)[percent_concat.asm]: Macro called from
E       /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar1/percent_concat.asm(8): Main line code
E     
E   assert 1 == 0
E    +  where 1 = CompletedProcess(args=['/home/eo/src/vc-linux-wt/command/build/kermit-toolchain/jwasm', '-q', '-Zm', '-bin', '-Fo=new....ro called from\n  /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar1/percent_concat.asm(8): Main line code\n').returncode
_ test_legacy_masm_tool_fixes_are_required_without_source_edits[empty_segment_alignment] _
tests/test_msdos_build.py:178: in test_legacy_masm_tool_fixes_are_required_without_source_edits
    assert result.returncode == 0, result.stderr.decode(errors="replace")
E   AssertionError: /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar2/empty_segment_alignment.asm(3) : Error A2078: Segment definition changed: code_seg, alignment
E     
E   assert 1 == 0
E    +  where 1 = CompletedProcess(args=['/home/eo/src/vc-linux-wt/command/build/kermit-toolchain/jwasm', '-q', '-Zm', '-bin', '-Fo=new....y_masm_tool_fixes_ar2/empty_segment_alignment.asm(3) : Error A2078: Segment definition changed: code_seg, alignment\n').returncode
_ test_legacy_masm_tool_fixes_are_required_without_source_edits[parity_linefeed] _
tests/test_msdos_build.py:178: in test_legacy_masm_tool_fixes_are_required_without_source_edits
    assert result.returncode == 0, result.stderr.decode(errors="replace")
E   AssertionError: /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar3/parity_linefeed.asm(4) : Error A2150: Missing operator in expression
E     
E   assert 1 == 0
E    +  where 1 = CompletedProcess(args=['/home/eo/src/vc-linux-wt/command/build/kermit-toolchain/jwasm', '-q', '-Zm', '-bin', '-Fo=new....f-eo/pytest-48/test_legacy_masm_tool_fixes_ar3/parity_linefeed.asm(4) : Error A2150: Missing operator in expression\n').returncode
_ test_legacy_masm_tool_fixes_are_required_without_source_edits[if2_forward_external] _
tests/test_msdos_build.py:178: in test_legacy_masm_tool_fixes_are_required_without_source_edits
    assert result.returncode == 0, result.stderr.decode(errors="replace")
E   AssertionError: /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar4/if2_forward_external.asm(10) : Error A2143: Symbol redefinition: later
E     
E   assert 1 == 0
E    +  where 1 = CompletedProcess(args=['/home/eo/src/vc-linux-wt/command/build/kermit-toolchain/jwasm', '-q', '-Zm', '-bin', '-Fo=new....eo/pytest-48/test_legacy_masm_tool_fixes_ar4/if2_forward_external.asm(10) : Error A2143: Symbol redefinition: later\n').returncode
_ test_legacy_masm_tool_fixes_are_required_without_source_edits[contextual_labels] _
tests/test_msdos_build.py:178: in test_legacy_masm_tool_fixes_are_required_without_source_edits
    assert result.returncode == 0, result.stderr.decode(errors="replace")
E   AssertionError: /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(4) : Error A2209: Syntax error: OUT
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(5) : Error A2209: Syntax error: IFDIF
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(6) : Error A2209: Syntax error: PAGE
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(7) : Error A2209: Syntax error: TEST
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(10) : Error A2047: Syntax error: Unexpected colon
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(11) : Error A2209: Syntax error: IFDIF
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(12) : Error A2047: Syntax error: Unexpected colon
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm(13) : Error A2047: Syntax error: Unexpected colon
E     /tmp/pytest-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm : Error A2080: Block nesting error: if-else
E     
E   assert 1 == 0
E    +  where 1 = CompletedProcess(args=['/home/eo/src/vc-linux-wt/command/build/kermit-toolchain/jwasm', '-q', '-Zm', '-bin', '-Fo=new....t-of-eo/pytest-48/test_legacy_masm_tool_fixes_ar5/contextual_labels.asm : Error A2080: Block nesting error: if-else\n').returncode
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_legacy_masm_tool_fixes_are_required_without_source_edits[escaped_directives]
FAILED tests/test_msdos_build.py::test_legacy_masm_tool_fixes_are_required_without_source_edits[percent_concat]
FAILED tests/test_msdos_build.py::test_legacy_masm_tool_fixes_are_required_without_source_edits[empty_segment_alignment]
FAILED tests/test_msdos_build.py::test_legacy_masm_tool_fixes_are_required_without_source_edits[parity_linefeed]
FAILED tests/test_msdos_build.py::test_legacy_masm_tool_fixes_are_required_without_source_edits[if2_forward_external]
FAILED tests/test_msdos_build.py::test_legacy_masm_tool_fixes_are_required_without_source_edits[contextual_labels]
6 failed in 0.14s
```

## options

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=options \
  tests/test_msdos_build.py::test_force_include_resolves_keyword_collisions_without_vendor_edits --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_____ test_force_include_resolves_keyword_collisions_without_vendor_edits ______
tests/test_msdos_build.py:196: in test_force_include_resolves_keyword_collisions_without_vendor_edits
    subprocess.run([str(builder.ensure_jwasm()), "-q", "-Zm", "-bin", "-Fo=new.bin",
docs/verification/brief30_build_defects.py:73: in run
    return original(command, *args, **kwargs)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
/home/eo/.local/share/uv/python/cpython-3.13-linux-x86_64-gnu/lib/python3.13/subprocess.py:577: in run
    raise CalledProcessError(retcode, process.args,
E   subprocess.CalledProcessError: Command '['/home/eo/src/vc-linux-wt/command/build/msdos-toolchain/jwasm', '-q', '-Zm', '-bin', '-Fo=new.bin', '/tmp/pytest-of-eo/pytest-47/test_force_include_resolves_ke0/keywords.asm']' returned non-zero exit status 1.
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_force_include_resolves_keyword_collisions_without_vendor_edits
1 failed in 0.06s
```

## legacy-tool-pass

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=legacy-tool \
  tests/test_msdos_build.py::test_if1_if2_are_reexecuted_on_later_passes --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_________________ test_if1_if2_are_reexecuted_on_later_passes __________________
tests/test_msdos_build.py:209: in test_if1_if2_are_reexecuted_on_later_passes
    assert (tmp_path / "new.bin").read_bytes() == bytes.fromhex("02c3")
E   AssertionError: assert b'\x01\x02\xc3' == b'\x02\xc3'
E     
E     At index 0 diff: b'\x01' != b'\x02'
E     Use -v to get more diff
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_if1_if2_are_reexecuted_on_later_passes
1 failed in 0.04s
```

## legacy-tool-alias

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=legacy-tool \
  tests/test_msdos_build.py::test_reassigned_relocatable_return_alias_uses_current_pass_value --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_______ test_reassigned_relocatable_return_alias_uses_current_pass_value _______
tests/test_msdos_build.py:228: in test_reassigned_relocatable_return_alias_uses_current_pass_value
    assert (tmp_path / "new.bin").read_bytes() == bytes.fromhex("c3803e00000074f890c3803e01000075f8c3")
E   AssertionError: assert b'\xc3\x80>\x...\x00u\xfe\xc3' == b'\xc3\x80>\x...\x00u\xf8\xc3'
E     
E     At index 7 diff: b'\xfe' != b'\xf8'
E     Use -v to get more diff
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_reassigned_relocatable_return_alias_uses_current_pass_value
1 failed in 0.04s
```


## sort-allocation

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=sort-allocation \
  tests/test_msdos_build.py::test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_____ test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata ______
tests/test_msdos_build.py:155: in test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata
    assert struct.unpack_from("<2H", final, 10) == (0, 1)
E   assert (0, 65535) == (0, 1)
E     
E     At index 1 diff: 65535 != 1
E     Use -v to get more diff
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata
1 failed in 1.75s
```

## sort-payload

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=sort-payload \
  tests/test_msdos_build.py::test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_____ test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata ______
tests/test_msdos_build.py:157: in test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata
    assert [i for i, (a, b) in enumerate(zip(raw, final)) if a != b] == [12, 13]
E   assert [12, 13, 32] == [12, 13]
E     
E     Left contains one more item: 32
E     Use -v to get more diff
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata
1 failed in 1.79s
```

## sort-header-guard

```sh
env PYTHONPATH=docs/verification:. .venv/bin/python -m pytest -q \
  -p brief30_build_defects --msdos-defect=sort-header-guard \
  tests/test_msdos_build.py::test_sort_exemod_rejects_invalid_headers_and_insufficient_allocation --tb=short --show-capture=no
```

```text
F                                                                        [100%]
=================================== FAILURES ===================================
_____ test_sort_exemod_rejects_invalid_headers_and_insufficient_allocation _____
tests/test_msdos_build.py:175: in test_sort_exemod_rejects_invalid_headers_and_insufficient_allocation
    with pytest.raises(ValueError):
         ^^^^^^^^^^^^^^^^^^^^^^^^^
E   Failed: DID NOT RAISE ValueError
=========================== short test summary info ============================
FAILED tests/test_msdos_build.py::test_sort_exemod_rejects_invalid_headers_and_insufficient_allocation
1 failed in 1.77s
```
