# Brief 26: dial the BBS from VC with MS-DOS Kermit and a virtual modem

Read `CLAUDE.md`, `docs/plans/2026-10-01-bbs-experience.md` (decided) and the GW-BASIC and Rogue
briefs (`docs/briefs/11-gwbasic.md`, `docs/briefs/16-rogue.md`) first. Toolchain as before:
`export WATCOM=$PWD/build/openwatcom`, emsdk on disk, `build/emcache` warm. No network, no browser.
I test against the live BBS and in real browsers afterwards.

## Goal

From VC, a visitor presses Enter on `H:\BBS.TAK`. MS-DOS Kermit starts, its script dials
`ATDT 555-1992`, the modem answers `CONNECT 14400`, and the notanemulator BBS login screen appears
in Kermit's own terminal emulator. Typing works both ways. `+++` then `ATH` hangs up with
`NO CARRIER`. Exiting Kermit returns to VC. Both the Linux build and the browser build.

## 1. MS-DOS Kermit 3.15, unedited, translated

- The sources and the release are at `build/mskermit/` (copied from my tools cache; the brief
  worktree can read it): `msk315src.zip` (SHA-256
  `1d897b06f1a6fa6a14bac401c20cc1b12b04e9f330ff4dc45f8d826d2aa60e72`), `msk315.zip` (SHA-256
  `435aa3db8275fdbf2462e6bb6d6e115815d4769217cda0a7f6466d917b6d47b2`) and the licence page
  `licensing.html`. They came from the Wayback Machine copy of
  `https://www.columbia.edu/kermit/ftp/archives/`, because Columbia's server refuses direct
  downloads. Record that, both hashes and the dates in `third_party/mskermit/UPSTREAM`.
- Vendor the source files unedited into `third_party/mskermit/`. Write `LICENSE` as the Revised
  3-Clause BSD licence with the copyright line from `mssker.asm` ("Copyright (C) 1982, 1997,
  Trustees of Columbia University in the City of New York"), and quote the licensing page's
  sentence that puts MS-DOS Kermit under that licence since July 2011. Keep `licensing.html` there
  as the evidence.
- Build the pure-assembly variant: define `no_network` on the assembler command line, not in the
  source, so the C TCP/IP stack (`msn*`) is left out, as `mssdef.h` describes. Use JWasm and JWlink
  in MASM 5.1 mode, as for VC. If JWasm cannot assemble something unedited, fix the tool side or
  its options, never the source, and say what it was.
- `make kermit` builds `KERMIT.EXE` reproducibly and the translator emits its image. Translate it
  and every distinct instruction against unicorn, as for VC and GW-BASIC. VC's, GW-BASIC's and
  the others' generated C stay byte-identical; prove it with `cmp`.
- In the browser, Kermit is a lazily loaded side module like the others, with a hashed name.
  `tests/web_size.mjs` must still pass.

## 2. A serial port and a Hayes modem in the machine

- An 8250/16450 UART at COM1, ports 3F8h to 3FFh: THR/RBR, IER, IIR, LCR with DLAB and the divisor
  latch, MCR, LSR and MSR with real bit meanings. Received bytes raise IRQ 4 through the 8259
  (INT 0Ch) when IER allows, delivered at dispatch boundaries like the timer, with EOI at port 20h.
  Also BIOS INT 14h functions 0 to 3 on the same port. Read `mssser.asm` and `msxibm.asm` to see
  exactly which registers and interrupts Kermit uses, and implement what a real 8250 does for
  those, not more.
- Behind the UART sits a Hayes-compatible modem in command mode: `AT`, `ATZ`, `ATE0/1`, `ATV0/1`,
  `ATQ0/1`, `ATM0/1`, `ATI`, `ATH`, `ATO`, `ATA` answers `NO CARRIER`, `ATDT`/`ATDP`/`ATD` with a
  number, and `+++` with the standard one-second guard time. Result codes `OK`, `ERROR`,
  `CONNECT 14400`, `NO CARRIER`, `BUSY`, `NO ANSWER`, verbose and numeric. DCD, DSR and CTS in the
  MSR follow the call. DTR drop hangs up. Pace data to about 14400 bps in both directions, as the
  CONNECT string promises.
- A phone book maps numbers to endpoints. `555-1992` is the notanemulator BBS: in the browser
  `wss://axis.tail85247.ts.net:8443/`; on Linux a TCP host and port set by
  `VC_MODEM_555_1992=host:port`, unset by default (then `NO ANSWER`, explained in the README). Any
  other number answers `NO ANSWER`. Keep the phone book as data in one place.
- The BBS speaks telnet over both transports. Captured bytes from the live server are in
  `build/mskermit/enigma-connect-2026-10-02.bin`; copy them into `tests/fixtures/`. The
  modem handles telnet itself, as a real telnet modem does: answer WILL/DO for BINARY, SGA and
  ECHO sensibly, refuse the rest, answer TTYPE with `ANSI` and NAWS with 80×25, unescape IAC IAC,
  and escape 0xFF going out. Kermit itself never sees telnet bytes.
- Browser: the page opens the WebSocket on dial, with binary frames. Linux: a non-blocking TCP
  socket. Both flow through the same modem code. A closed connection gives `NO CARRIER`.

## 3. Running it

- `KERMIT.EXE` on H: in the browser, installed next to the others on Linux, on the DOS `PATH`.
- `H:\BBS.TAK`, a Kermit script of our own: set port COM1 and speed 57600, choose the terminal
  type that best shows ANSI BBS screens (read Kermit's `SET TERMINAL TYPE` options in the source),
  dial `ATDT555-1992`, wait for `CONNECT`, then `CONNECT`. Add a `TAK` association in VC's
  extension file so Enter on `BBS.TAK` runs `kermit take BBS.TAK`.
- `H:\KERMIT.TXT`: how to dial, how to get back to Kermit's prompt (its escape key), how to hang
  up, how to quit. Update `web/README.TXT` and the README.
- When Kermit exits, VC comes back with its panels redrawn, as for the other programs.

## Gates

- Unit tests: UART registers including DLAB, LSR data-ready and THR-empty, MSR deltas, IIR
  priorities, IRQ 4 delivery and EOI; INT 14h; the Hayes parser for every command and result code;
  `+++` guard time; the telnet layer against the captured fixture, checking the exact replies.
- An e2e test in a pty on Linux: a local fake BBS on 127.0.0.1 that replays the captured fixture
  and echoes input. VC starts, Enter on `BBS.TAK`, the screen shows `ENiGMA` and `BBS version`
  from the fixture, typed text reaches the fake BBS, `+++` and `ATH` print `NO CARRIER`, `EXIT`
  returns to VC's panels.
- The web smoke test does the same through an injected fake WebSocket transport, the way it
  already injects program fetches.
- `make test`, `make web`, `make test-web` green. Plant a defect for each new gate in turn, watch
  it go red, restore it, and report each one.

## Done means

All gates green. The summary lists files changed, the first-load size, Kermit's module size, which
JWasm options or tool fixes Kermit needed, and three weaknesses of your own work, each with a real
`file:line`. Do not commit.
