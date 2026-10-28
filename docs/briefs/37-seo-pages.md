# Brief 37: search and AI pages for notanemulator.com

Today a crawler sees 70 words on notanemulator.com, and none of them name GW-BASIC, Rogue, Hack,
MS-DOS 2.0, VZ Editor, Kermit or bootLogo. There is no robots.txt, sitemap.xml, llms.txt, social
card, canonical URL or structured data. Google lists the GitHub release, not the site. This brief
adds one static page per program, in English and Russian, plus the files search engines and AI
assistants read, and a CI gate that keeps all of it true.

The page text is already written in `site/pages.toml`: English by Claude from the README's facts, Russian by Gemini. Do not
write or edit any Russian or English prose yourself. If the text needs to change, say so in your
summary. Only `run`, `modules` and structural fields may be added to that file (see section 3).

Stdlib Python only (`tomllib`, `html`, `json`), same style as `tools/web_*.py`. Run tools with
`.venv/bin/python`. Same emcc toolchain as the earlier web briefs.

## Toolchain

    export WATCOM=$PWD/build/openwatcom
    export EMSDK=$HOME/src/vc-linux-wt/emsdk
    export EM_CONFIG=$EMSDK/.emscripten EM_CACHE=$PWD/build/emcache
    export PATH=$EMSDK/upstream/emscripten:$EMSDK/node/20.18.0_64bit/bin:$PATH

`build/openwatcom` and a warm `build/emcache` are in place; `build/gen/` is not built yet, so
`make` first. Your sandbox blocks loopback sockets and git writes: run everything else, do not
commit.

## 1. Generator: `tools/site_pages.py`

Renders `site/pages.toml` into `build/web` as part of `make web` (so the Pages deploy carries it):

- `/programs/index.html` (English hub) and `/ru/index.html` (Russian hub).
- `/<slug>/index.html` (English) and `/ru/<slug>/index.html` (Russian) for every `[[program]]`.
- `/robots.txt`: allow everything, plus `Sitemap: https://notanemulator.com/sitemap.xml`.
- `/sitemap.xml`: the home page, both hubs and every program page, with `xhtml:link`
  `hreflang` alternates for each en/ru pair. No `lastmod`: a date that changes on every build is
  worse than none.
- `/llms.txt` in the llmstxt.org format: `# notanemulator`, a one-paragraph `>` summary taken from
  the English hub, then `## Programs` with one `- [Name](url): description` line per English page,
  then `## Source` with the GitHub repo and the Zenodo DOI.
- `/social-preview.png`: a copy of `docs/social-preview.png` (1200x630).

Every page:
- `<html lang>` matching its language, one `<title>`, `<meta name="description">`,
  `<link rel="canonical">` to its own absolute URL, and `hreflang` links for `en`, `ru` and
  `x-default` (the English page). The hubs pair `/programs/` with `/ru/`.
- Open Graph and Twitter card tags with the page's own title and description and
  `https://notanemulator.com/social-preview.png`.
- JSON-LD: the program's `schema_type` (`SoftwareApplication` or `VideoGame`) with `name`, `author`
  (a Person per name, or an Organization for Microsoft and Columbia University), `dateCreated` (the
  first year of `years`), `license` (the GitHub blob URL of `license_path`), `isBasedOn`
  (`source_url`), `applicationCategory`, `operatingSystem` ("MS-DOS, run in a web browser as
  WebAssembly"), `isAccessibleForFree: true`, `inLanguage` and `url`. Plus a `BreadcrumbList`:
  home, the hub in that language, the page. The hubs carry a `WebSite` and an `ItemList` of the
  programs.
- A visible `h1`, the body paragraphs (escape everything; convert only `[text](url)` to links), a
  Play button, the Linux command (`linux` field, as `vc` then the command on VC's command line,
  except `vc` itself), a "Source and licence" line linking `source_url` and the licence file, a
  list of the other programs, and a link to the other language.
- A Play link to `/?run=<slug>`; for Volkov Commander, Play is `/` and a second button runs
  `also_run`. Russian pages use the same targets.
- The GoatCounter tag exactly as `web/index.html` loads it, so page visits are counted.
- No JavaScript otherwise, and no external fonts or CSS. Small inline CSS in the site's DOS look:
  black background, the VGA palette's blue `#0000aa` header band, cyan `#55ffff` links, readable
  system font for body text at a comfortable measure, and fine at 360px wide.

## 2. The home page (`web/index.html`)

- Title: `Volkov Commander online: DOS programs, not an emulator`. Description: `Run Volkov
  Commander, GW-BASIC, Rogue, Hack, MS-DOS 2.0, VZ Editor, Kermit and bootLogo in your browser,
  translated from 8086 code to WebAssembly. No emulator.`
- Canonical `https://notanemulator.com/`, Open Graph and Twitter tags as above, and JSON-LD for a
  `WebSite` plus a `SoftwareApplication` for Volkov Commander 4.99.09.
- One visible footer link to the program list: English `Programs` to `/programs/`, Russian
  `Программы` to `/ru/`, Ukrainian `Програми` to `/programs/`, chosen by `pageText` in
  `web/vc-language.js` like the footer's other strings. The phone and desktop layouts must keep
  every existing `tests/test_web_layout.mjs` and keypad assertion. Update fixtures only by the
  measured height of the new link, and say so.
- The first-load budget in `tests/web_size.mjs` must still pass. Report the before and after
  gzipped first-load bytes.

## 3. Play: `/?run=<slug>`

- The generator writes `build/web/vc-programs.js`, an ES module exporting a frozen map from slug to
  that program's `run` lines. `vc-web.js` imports it with a dynamic `import()` only when the page
  URL has a `run` parameter, so a normal first load fetches nothing new.
- An unknown slug does nothing. Text from the URL is never typed: only lines from the map are.
- Each line is typed, followed by Enter, only when VC idles at its keyboard: the input queue is
  empty and `readInput` has been called at least twice since the last byte was taken. The next
  line waits for the same condition after the previous command has finished. VC drops keys typed
  while it redraws, which cost several flaky tests on 2026-10-05, so never type on a timer.
- Choose `run` lines that work from VC's start directory and change the TOML only if a line does
  not work. Prefer one line per program.
- Add `modules = [...]` to each `[[program]]`: the lazy side-module basenames that program owns
  (for example `rogue`; `command`, `edlin`, `debug`, `find`, `more`, `sort`, `fc` for MS-DOS 2.0;
  `vc405`, `vcsetup405` for Volkov Commander). The gate in section 4 uses them.

## 4. The gate: `tests/test_site.mjs`, run by `make test-web`

It fails, naming the page and the field, when any of these breaks:
1. Every program has `en` and `ru` tables with `title` (at most 60 characters), `description`
   (70 to 160), `h1` and `body`. No Russian field equals its English one.
2. Coverage: every lazy side-module the web build produces is owned by exactly one page's
   `modules`. A new program without a page goes red.
3. Every page in `sitemap.xml` exists in `build/web`, and every generated page is in the sitemap
   and in `llms.txt` (English pages). Titles are unique across all pages.
4. Each page has exactly one title, a description, a canonical equal to its own URL, reciprocal
   `hreflang` pairs, parseable JSON-LD of the expected types, and an `og:image` whose file exists
   at 1200x630.
5. Every same-site link and asset on every page resolves to a file in `build/web`. Every
   `license_path` exists in the repository. Every external link is `https`.
6. Every Play link targets `/` or `/?run=<slug>` with that slug in `vc-programs.js`.
7. In `tests/web_smoke.mjs`, add a mode that boots VC once per slug with that slug's run lines and
   waits for the program's own screen (Rogue's status line, Hack's, GW-BASIC's `Ok`, COMMAND's
   prompt, VZ's screen, Kermit's prompt or `CONNECT`, bootLogo's graphics, VC 4.05's panels). Wire
   it into `make test-web`. Kermit may use the existing fake modem path.

Show each check red at least once against a deliberate defect, then restore: a missing Russian
title, a side-module no page owns, a broken internal link, a page missing from the sitemap, a
`run` line that never reaches its program. Record each in your summary.

## 5. Docs

README: a short section saying the site's pages come from `site/pages.toml`, how to add a program
page, and that Russian text comes from Gemini, never from Claude or Codex. `CLAUDE.md`: one line
with the same rule.

## Done means

`make web test-web` and `make test` pass. End with a summary that lists the files changed, the five
red-then-green gate results, the first-load bytes before and after, any layout fixture you changed
and why, and any page text you believe is wrong. Do not run any git command that writes; I commit.
