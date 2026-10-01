# A BBS experience on notanemulator.com

**Type:** Decision doc
**Status:** exploring
**Date:** 2026-10-01

## Goal

Eugene, 2026-10-01: "i think it should be something like a bbs experience, don't know how to
describe it. like you log in, but other user can message. also would be great to save the state,
but later".

This reverses one earlier choice. `docs/plans/2026-10-01-games.md` called notanemulator.com a
static toy, "deliberately not a hosted service". Logging in and messaging other people needs a
server, accounts, and some answer to spam and abuse. That is exposure, so it is gated on purpose,
reader and success first.

## Criteria, fixed before the options

1. It feels like dialling into a 1990s BBS: a login, a menu, other people present, messages.
2. Other visitors can see and answer you, in real time or later.
3. VC and the programs keep running in the browser, translated, as today.
4. Abuse stays manageable for one person: rate limits, delete, ban.
5. It runs on infrastructure Eugene already has, at near-zero cost.
6. Saved state per user (the H: drive) can come later without a redesign.

## Options so far

| Option | Strongest objection | Fails when |
|---|---|---|
| A. A small message server of our own (WebSocket), and a BBS front door in the page: login, who's online, message boards, private mail, paging. VC and the games become its "doors", as BBSes ran external programs. | We write and run a server, and own its moderation. | It attracts spam faster than one person can clean it. |
| B. Host an existing open-source BBS, ENiGMA½ (BSD-2, Node.js, active) or Synchronet (C++, 30 years old), and give the page a terminal that dials it. | The BBS is a modern program, not translated DOS, and two worlds meet in one page. | Its ANSI menus and our VC screen feel like two products glued together. |
| C. Maximus BBS, translated like VC, run as a multi-node server on Linux, dialled from the page. | The most authentic, and by far the most work: Maximus is the biggest program on the list, and multi-node DOS sharing is new runtime work. | The DOS build cannot be restored, the risk already noted in the games doc. |

Not yet weighed: where the server runs (the K3s cluster `axis`, box, or an edge service), how
people log in (a handle and password, or GitHub), and what saved state means.

## Questions for Eugene

1. (Criterion 1) What does a visitor see first: the BBS login, with VC and games as doors behind
   it, or VC as today, with the BBS as something you dial from inside it?

## Answers

1. Eugene, 2026-10-01, after inline mocks of both flows: "B: VC first, dial the BBS". VC opens as
   today. A terminal program on H: dials the BBS with a modem moment (`ATDT`, `CONNECT 14400`),
   then the BBS login, boards, mail and paging happen there.

## How the dial works (checked 2026-10-01)

- **The terminal program is MS-DOS Kermit**, from Columbia University. It has been open source
  under the revised BSD-3 licence since 1 July 2011, and its source is assembly (`ms*.asm`). It
  is translated like VC and has real VT100/ANSI emulation, Hayes dialling and file transfer.
- **The modem is ours.** The runtime emulates a Hayes modem on COM1, at the 8250 UART ports and
  INT 14h. `ATDT 555-1992` opens a WebSocket from the page, or a TCP connection on Linux, to the
  BBS server. It answers `CONNECT 14400` and passes bytes both ways. `+++ATH` hangs up.
- So the BBS server is the only new hosted piece. The page stays static on GitHub Pages.

## Questions for Eugene, continued

2. (Criteria 2, 4, 5) Which BBS runs on the server: ENiGMA½ (BSD-2, maintained, multi-node with
   boards, mail and paging built in), a small server of our own, or Maximus translated like VC?

2. Eugene, 2026-10-01: ENiGMA½ answers the call.
3. Eugene, 2026-10-01: it runs on the `axis` k3s node, at `bbs.notanemulator.com`.

## Decision — 2026-10-01

- **Client:** MS-DOS Kermit, translated, on H:. A virtual Hayes modem on COM1 bridges `ATDT` to a
  WebSocket in the page, or TCP on Linux, at `bbs.notanemulator.com`.
- **Server:** ENiGMA½ on `axis` behind Traefik with TLS: accounts, boards, private mail, who's
  online, paging and chat. Eugene is the sysop.
- **Later:** saved state per user (H: tied to the BBS account) and translated BBS doors.

4. Eugene, 2026-10-01: reach the BBS through Tailscale Funnel today, at
   `wss://axis.tail85247.ts.net:8443`, since the Oracle API key on axis returns 401 and opening
   ports needs a console sign-in. `bbs.notanemulator.com` can follow once ports 80 and 443 are open.
   Funnel on axis already serves another app on 443, which stays untouched.

## Outcome so far — 2026-10-01

- The server side is live. ENiGMA½ 0.5.1-beta runs on `axis` (`infra/bbs/`). Its WebSocket
  answers publicly at `wss://axis.tail85247.ts.net:8443/` through Tailscale Funnel. Telnet stays
  inside the tailnet.
- The sysop account `eugene` (user 1) was registered before anything was exposed, since the first
  account to apply becomes the sysop.
- Still to build: MS-DOS Kermit translated, and the virtual modem bridging `ATDT` to that WebSocket.
