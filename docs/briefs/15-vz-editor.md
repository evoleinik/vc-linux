# Brief 15: VZ Editor as VC's editor

Read `docs/plans/2026-10-01-games.md` (the "A text editor" section, decided as option J) and
`CLAUDE.md` first. Follow the shape of briefs 11 and 14: real DOS files, EXEC by bytes, both
builds.

Goal: VZ Editor 1.6, US build, runs as a translated DOS program. In the browser, F4 on a file
opens it in VZ, the user edits and saves, and VZ returns to VC with the file changed on H:. On
Linux, F4 keeps using `$EDITOR`, and falls back to VZ when `$EDITOR` is unset. Typing `vz FILE`
on VC's command line works in both builds.

You have no network. Everything is on disk.

## What is already here

- `third_party/vzeditor/`: VZ Editor by c.mos (Village Center), BSD-3, from
  github.com/vcraftjp/VZEditor (commit in `UPSTREAM`). `SRC/` holds 29 modules of 8086 assembly
  for MASM 5.1 or OPTASM. `SRC/VZ.MAK` and `SRC/MK.BAT` are the original build; the US target
  is `/dUS`. `VZ-IBM/US/VZUS.COM` is the shipped US binary, and `INSTUS.LST` lists its files.
- Measured on 2026-10-01 with our `tools/jwasm/jwasm -Zm -Cp -DUS`: once include names match
  case-insensitively (the sources say `include vz.inc`, the file is `VZ.INC`), 25 of 29 modules
  assemble cleanly. Nine errors remain in four modules:
  - `KEYIBM.ASM`: anonymous `@@` labels.
  - `SCRN.ASM`: includes `scrnIBM.asm` with that exact case.
  - `STRING.ASM`: a `dd` form.
  - `MSG.ASM`: a `label byte` form.
- `tools/jwlink/jwlink` links multi-module DOS programs. The translator already places
  multi-module listings by the link map (brief 11).

## 1. Build VZUS.COM, byte-identical to the shipped one

`make vz` builds `build/vz/VZ.COM` and its listings and map. Never edit the vendored sources.
Fix the dialect gaps at build time, the way the GW-BASIC fork uses `jwasmify.awk`: a small
preprocessing step, plus a case-insensitive copy of the include files. The result must equal
`third_party/vzeditor/VZ-IBM/US/VZUS.COM` byte for byte. If it cannot, explain every differing
byte, and ship only differences that encode the same instruction. Check whether the shipped
binary carries installed settings, since `INST.ASM` patches configuration into the COM file.
Match whatever the user would have run.

## 2. Translate and run it

- The translator's multi-module path emits its image. `make test-translator` covers every
  distinct VZ instruction against unicorn. VC's, GW-BASIC's and bootLogo's generated C stay
  byte-identical; prove it with `cmp`.
- Implement whatever BIOS and DOS services VZ needs, found by running it. If it probes EMS
  (INT 67h), a correct "no EMS" answer is enough.
- Install `VZ.COM` as a real file next to `GWBASIC.EXE`, with any `.DEF` files the US build
  loads at start-up, so menus and help are English. Matching stays by bytes.

## 3. F4 and the command line

- Browser: `vc-edit FILE` runs `VZ.COM FILE` through the same in-process EXEC as `gwbasic`, never
  a shell. Replace the browser's "No editor" message.
- Linux: `vc-edit` keeps opening `$EDITOR` with the resolved host path, as today. When `$EDITOR`
  is unset or empty, it runs VZ the same way as the browser.
- A file name must never reach `/bin/sh` as text. Keep the existing injection tests green, and add
  one for a file whose name contains `;touch PWNED` opened with F4 when `$EDITOR` is unset.

## Gates

- `make test` green, with byte-identical generated C for the earlier programs.
- `make vz` reproduces `VZUS.COM`, or the summary explains each differing byte.
- E2e tests in a pty, with `$EDITOR` unset:
  1. F4 on a text file opens VZ showing its first line.
  2. Type a line, save with VZ's own save key, and quit. The file on disk holds the new line,
     and VC's panels are back.
  3. `vz NEW.TXT` on the command line creates a file after a save.
  4. The injection test from section 3.
- `make web` and `make test-web` green. The node smoke test presses F4 on `README.TXT`, edits,
  saves, quits, and reads the change back from MEMFS. Update `web/README.TXT` with VZ's basic keys.
- Plant a defect for each new gate in turn, watch it go red, restore it, and report each one.

## Done means

All gates green. Your final summary lists files changed, the wasm gzip size and its growth, and
VZ's save and quit keys. It names three weaknesses of your own work, each with a real
`file:line`. Do not commit; I commit.
