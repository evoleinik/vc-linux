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
