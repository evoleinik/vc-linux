# Brief 16: native play and packaging verification

Date: 2026-10-01. These results used the 202,382-byte safe-save `ROGUE.EXE`,
SHA-256 `fa2aca4c68edefe983d13bb2323aaf85f1a061a3fa0f936ff2d285455aca3553`.
The subsequent 32-bit PDCurses attribute and gold/score executable has now
passed the final aggregate: 149 native end-to-end/installation cases and all
24 browser smoke stages. See [the aggregate record](16-rogue.md) for that
final image, full logs and the separate unresolved CI prerequisite.

## Healthy baseline

```sh
.venv/bin/python -m pytest -q tests/test_rogue_e2e.py tests/test_install_e2e.py
```

Result before the additional rare-trap fixtures: **37 passed in 29.54s**.
The native tests exercise typed commands through COMSPEC and INT 2Eh, Enter
on the EXE, full-byte matching under another name, rejection of a changed
image, the real level-1 dungeon/status, movement, all option callbacks,
quit/parent survival, current-directory saves/scores, exact visible game
restoration, and preservation of a truncated save. Installer tests check
atomic/stable installation and the browser game's exact executable/licenses.
The save comparison covers visible map/player/status; DOS port tests cover
the serialized types and attributes separately.

Log: `build/rogue-verification/native-full.txt`.

## Deliberate defects: red, then healthy recovery

Native faults were compiled as isolated runtime include wrappers under
`build/rogue-verification/`; canonical runtime sources, EXE and generated C
were unchanged. Each red command set `VC_TEST_BINARY` to that fault binary;
recovery ran the same test against the untouched `build/vc`.

| Fault | Exact failing gate in `tests/test_rogue_e2e.py` | Red / healthy recovery |
|---|---|---|
| Remove Rogue from the EXEC image registry | `test_rogue_command_dungeon_and_status[rogue-comspec]` | 1 failed, 20.44s / 1 passed, 0.58s |
| Consume Rogue movement keys without delivering them | `test_rogue_movement_moves_player` | 1 failed, 8.78s / 1 passed, 0.88s |
| Consume `y` at the visible quit confirmation | `test_rogue_quit_restores_panels[comspec]` | 1 failed, 15.98s / 1 passed, 2.00s |
| Make `access(rogue.sav)` report missing after a real save | `test_rogue_save_restores_same_game_in_current_directory` | 1 failed, 10.01s / 1 passed, 2.60s |

Full compile commands, pytest commands, diagnostics and exit codes are in
`build/rogue-verification/{startup,movement,quit,restore}-defect.txt`.
The injected faults are explicitly identified in the relevant VC logs.

The packaging fault temporarily removed `GAMES/ROGUE.EXE` from
`tools/web_demo.py`. The exact
`tests/test_install_e2e.py::test_web_demo_installs_real_rogue_and_licenses_in_games`
gate failed (1 failed, 0.52s); restoring the entry passed (1 passed, 0.14s).
The source was restored. Log: `build/rogue-verification/package-defect.txt`.

The old unchecked-restore implementation was also tested against an actual
save truncated by 64 bytes. The preservation gate failed because the save
was deleted; the fixed implementation passes in the baseline above. Log:
`build/rogue-verification/truncated-save-old.txt`.

## Standalone full-byte identity regression

`build/rogue-verification/exec-identity-defect.c` includes the real DOS core
with a private `memcmp` wrapper. Only a comparison against the exact Rogue
embedded-data pointer and full image size is forced equal; comparisons for
the other images and the unit test's own assertions remain unchanged.

The isolated `tests/test_dos_exec.c` binary rejected this defect: exit 1 at
its Rogue changed-byte check, after logging an incorrectly accepted change
at byte 0 (`0x4C != 0x4D`). The untouched production `build/test_dos_exec`
then exited 0: **DOS EXEC: 1332 checks passed**. Core dumps were disabled.
Source hashes before/after were identical; no production/generated objects
were changed. Exact build/run commands and outputs:
`build/rogue-verification/exec-identity-red-green.txt`.

## Random-game and prompt review

Six independent repetitions of movement, save/restore and both quit paths
passed: **24/24 cases**, 44.59s of pytest time. The final two repetitions
include the rare-trap handling below. Log:
`build/rogue-verification/native-repeat-six.txt`.

Source review identified two legitimate hidden-trap outcomes on level 1:

- `T_TELEP` silently relocates the player and reveals `^` at the selected
  destination. Native and Node tests exempt the exact-one-step assertion
  only when that tile newly reveals `^` **and the player still changes**.
  Six pure fixtures reject dropped keys, arbitrary relocation, an old trap,
  an ordinary step and a missing player; all six pass.
- `T_RUST` can display `--More--` before `do_move` updates the player.
  Both tests now acknowledge only a visible prompt, at most eight times,
  while continuing to require movement. A synthetic screen-sequence fixture
  failed against the previous ordering (1 failed, 0.07s) and passed after
  the correction (1 passed, 0.05s). Log:
  `build/rogue-verification/prompt-order-red-green.txt`.

After both changes, the dropped-movement binary still fails the real movement
gate (1 failed, 8.84s). The healthy focused movement/save/fixture selection
passes (9 passed, 3.48s); exact commands/output are in
`build/rogue-verification/movement-trap-recheck.txt`.

`node --check tests/web_smoke.mjs` passes. The root agent owns the complete
Node smoke/fault campaign and the final rebuilt native/browser reruns.
