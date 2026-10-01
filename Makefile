# vc-linux build.
#   make images   assemble VC.COM and VC.OVL from asm/ with JWasm, plus listings
#   make gen      translate both images to C (build/gen/)
#   make          build the native binary build/vc
#   make test     run every test suite
#   make web      build the self-contained browser toy in build/web/
#   make test-web run its Node/MEMFS smoke test

JWASM   := tools/jwasm/jwasm
JFLAGS  := -q -Zg -Zne -DOFFICIAL
PY      := .venv/bin/python
B       := build
CC      ?= gcc
CFLAGS  ?= -O1 -g -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
ASM     := $(wildcard asm/*.ASM asm/*.INC)
FONT_HEADERS := $(wildcard third_party/font8x8/*.h)

all: $(B)/vc

include tools/gwbasic.mk
include tools/bootlogo.mk
include tools/rogue.mk
include tools/vz.mk

VZ_DEF_DIR := third_party/vzeditor/VZ-IBM
VZ_DEF_NAMES := VZFLE.DEF HELPE.DEF BLOCK.DEF PALET.DEF BW.DEF
VZ_DATA := $(B)/vz/VZ.DEF $(addprefix $(VZ_DEF_DIR)/,$(VZ_DEF_NAMES)) third_party/vzeditor/LICENSE
VZ_EMBED := VZ.COM=$(B)/vz/VZ.COM VZ.DEF=$(B)/vz/VZ.DEF $(foreach f,$(VZ_DEF_NAMES),$(f)=$(VZ_DEF_DIR)/$(f)) VZLIC.TXT=third_party/vzeditor/LICENSE

$(B)/vz/VZ.DEF: $(VZ_DEF_DIR)/VZIBM.DEF tools/vz_defaults.py
	$(PY) tools/vz_defaults.py $@

vz: $(B)/vz/VZ.DEF

images: $(B)/VC.COM $(B)/VC.OVL

$(B)/VC.COM: $(ASM)
	@mkdir -p $(B)
	cd asm && ../$(JWASM) $(JFLAGS) -bin -Fl=../$(B)/VC.COM.lst -Fo ../$(B)/VC.COM VC.ASM

$(B)/VC.OVL: $(ASM)
	@mkdir -p $(B)
	cd asm && ../$(JWASM) $(JFLAGS) -mz -Fl=../$(B)/VC.OVL.lst -Fo ../$(B)/VC.OVL VCOVL.ASM

.PHONY: all images gen test clean

# -Sg exposes PROC/USES/LOCAL prologues and RET epilogues omitted from the
# original image listings. Keep the images target untouched, and prove that
# requesting a richer listing does not change either linked image.
$(B)/gen/VC.COM.lst: $(B)/VC.COM $(ASM)
	@mkdir -p $(B)/gen
	cd asm && ../$(JWASM) $(JFLAGS) -Sg -bin -Fl=../$(B)/gen/VC.COM.lst.tmp -Fo ../$(B)/gen/VC.COM VC.ASM
	cmp $(B)/VC.COM $(B)/gen/VC.COM
	mv $(B)/gen/VC.COM.lst.tmp $(B)/gen/VC.COM.lst

$(B)/gen/VC.OVL.lst: $(B)/VC.OVL $(ASM)
	@mkdir -p $(B)/gen
	cd asm && ../$(JWASM) $(JFLAGS) -Sg -mz -Fl=../$(B)/gen/VC.OVL.lst.tmp -Fo ../$(B)/gen/VC.OVL VCOVL.ASM
	cmp $(B)/VC.OVL $(B)/gen/VC.OVL
	mv $(B)/gen/VC.OVL.lst.tmp $(B)/gen/VC.OVL.lst

$(B)/gen/vc_com.c: $(B)/VC.COM $(B)/gen/VC.COM.lst $(wildcard translator/*.py)
	$(PY) -m translator $(B)/VC.COM $(B)/gen/VC.COM.lst --name VC.COM --symbol image_vc_com -o $@

$(B)/gen/vc_ovl.c: $(B)/VC.OVL $(B)/gen/VC.OVL.lst $(wildcard translator/*.py)
	$(PY) -m translator $(B)/VC.OVL $(B)/gen/VC.OVL.lst --name VC.OVL --symbol image_vc_ovl -o $@

gen: $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c $(B)/gen/gwbasic.c $(B)/gen/bootlogo.c $(B)/gen/gwbasic_graphics.c $(B)/gen/rogue.c $(B)/gen/vz.c

test-translator: gen
	$(PY) -m pytest -q tests/test_translator_*.py

.PHONY: test-translator

# The terminal gate deliberately supplies its own Cpu and memory; CPU and DOS
# file-system implementations are independent work and must not be linked here.
$(B)/test_term: tests/test_term.c runtime/bios.c runtime/bios.h runtime/term.c runtime/term.h runtime/cpu.h runtime/hle.h $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Iruntime runtime/bios.c runtime/term.c runtime/cp866.c tests/test_term.c -o $@

test-term: $(B)/test_term
	./$(B)/test_term

$(B)/test_term_sanitize: tests/test_term.c runtime/bios.c runtime/bios.h runtime/term.c runtime/term.h runtime/cpu.h runtime/hle.h $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -Iruntime runtime/bios.c runtime/term.c runtime/cp866.c tests/test_term.c -o $@

test-term-sanitize: $(B)/test_term_sanitize
	./$(B)/test_term_sanitize

.PHONY: test-term test-term-sanitize

$(B)/test_cga: tests/test_cga.c runtime/bios.c runtime/bios.h runtime/cpu.h runtime/hle.h runtime/term.h $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic -Iruntime runtime/bios.c tests/test_cga.c -o $@

test-cga: $(B)/test_cga
	./$(B)/test_cga

.PHONY: test-cga

$(B)/test_dos_fs: tests/test_dos_fs.c runtime/dos_fs.c runtime/dos_fs.h runtime/cp866.c runtime/cp866.h runtime/cpu.h runtime/hle.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=c11 -Wextra -Iruntime tests/test_dos_fs.c runtime/dos_fs.c runtime/cp866.c -o $@

test-fs: $(B)/test_dos_fs
	./$(B)/test_dos_fs

$(B)/test_dos_fs-sanitize: tests/test_dos_fs.c runtime/dos_fs.c runtime/dos_fs.h runtime/cp866.c runtime/cp866.h runtime/cpu.h runtime/hle.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=c11 -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -Iruntime tests/test_dos_fs.c runtime/dos_fs.c runtime/cp866.c -o $@

test-fs-sanitize: $(B)/test_dos_fs-sanitize
	./$(B)/test_dos_fs-sanitize

.PHONY: test-fs test-fs-sanitize

$(B)/test_dos_exec: tests/test_dos_exec.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c $(wildcard runtime/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Iruntime tests/test_dos_exec.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c -o $@

test-exec: $(B)/test_dos_exec
	./$(B)/test_dos_exec

.PHONY: test-exec

$(B)/test_machine: tests/test_machine.c runtime/rt.c runtime/cpu.c runtime/bios.c $(wildcard runtime/*.h) $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime runtime/rt.c runtime/cpu.c runtime/bios.c tests/test_machine.c -o $@

test-machine: $(B)/test_machine
	./$(B)/test_machine

.PHONY: test-machine

$(B)/test_rt_process: tests/test_rt_process.c runtime/rt.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c runtime/bios.c $(wildcard runtime/*.h) $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime tests/test_rt_process.c runtime/rt.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c runtime/bios.c -o $@

test-process: $(B)/test_rt_process
	./$(B)/test_rt_process

.PHONY: test-process

# ---- the native binary -----------------------------------------------------
RT_SRC := runtime/rt.c runtime/dos_core.c runtime/main.c runtime/cpu.c runtime/dos_fs.c \
          runtime/cp866.c runtime/bios.c runtime/term.c
GEN_SRC := $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c $(B)/gen/gwbasic.c $(B)/gen/bootlogo.c $(B)/gen/gwbasic_graphics.c $(B)/gen/rogue.c $(B)/gen/vz.c $(B)/gen/files.c
GEN_OBJ := $(patsubst $(B)/gen/%.c,$(B)/obj/%.o,$(GEN_SRC))

$(B)/gen/files.c: $(B)/VC.COM $(B)/VC.OVL $(B)/gwbasic/GWBASIC.EXE $(B)/bootlogo/LOGO.COM $(B)/rogue/ROGUE.EXE $(B)/rogue/OWLIC.TXT third_party/rogue/LICENSE.TXT third_party/pdcurses/README.md $(B)/vz/VZ.COM $(VZ_DATA) data/VC.INI data/VC.EXT data/VCEDIT.EXT data/VC.HLP tools/embed.py Makefile
	@mkdir -p $(B)/gen
	$(PY) tools/embed.py $@ VC.COM=$(B)/VC.COM VC.OVL=$(B)/VC.OVL GWBASIC.EXE=$(B)/gwbasic/GWBASIC.EXE BOOTLOGO.COM=$(B)/bootlogo/LOGO.COM ROGUE.EXE=$(B)/rogue/ROGUE.EXE ROGUELIC.TXT=third_party/rogue/LICENSE.TXT PDCLIC.TXT=third_party/pdcurses/README.md OWLIC.TXT=$(B)/rogue/OWLIC.TXT $(VZ_EMBED) VC.INI=data/VC.INI VC.EXT=data/VC.EXT VCEDIT.EXT=data/VCEDIT.EXT VC.HLP=data/VC.HLP

$(B)/obj/%.o: $(B)/gen/%.c runtime/cpu.h runtime/image.h
	@mkdir -p $(B)/obj
	$(CC) $(CFLAGS) -Iruntime -c $< -o $@

$(B)/obj/files.o: runtime/rt.h

$(B)/vc: $(RT_SRC) $(wildcard runtime/*.h) $(FONT_HEADERS) $(GEN_OBJ)
	$(CC) $(CFLAGS) -std=gnu11 -Iruntime -o $@ $(RT_SRC) $(GEN_OBJ)

# The release binary: static and stripped, so it runs on any x86-64 Linux.
$(B)/vc-static: $(RT_SRC) $(wildcard runtime/*.h) $(FONT_HEADERS) $(GEN_OBJ)
	$(CC) -O2 -static -std=gnu11 -Iruntime -o $@ $(RT_SRC) $(GEN_OBJ)
	strip $@

BASIC_GAME_FILES := $(addprefix $(B)/games/,$(shell $(PY) tools/basic_games.py --names))
$(BASIC_GAME_FILES) &: tools/basic_games.py $(wildcard third_party/basic-computer-games/*/*.bas) third_party/basic-computer-games/LICENSE
	$(PY) tools/basic_games.py $(B)/games

games: $(BASIC_GAME_FILES)

test-e2e: $(B)/vc games
	$(PY) -m pytest -q tests/test_e2e.py tests/test_gwbasic_e2e.py tests/test_install_e2e.py tests/test_command_e2e.py tests/test_logo_e2e.py tests/test_rogue_e2e.py tests/test_vz_e2e.py

test-ini: $(B)/VC.OVL
	$(PY) -m pytest -q tests/test_setup_ini.py

test-rogue-build: rogue
	$(PY) -m pytest -q tests/test_rogue_build.py

test: test-translator test-fs test-exec test-machine test-process test-term test-cga test-ini test-rogue-build test-vz-build test-e2e

clean:
	rm -rf $(B)

.PHONY: test-e2e test-rogue-build games

# ---- the browser toy -------------------------------------------------------
# Needs Emscripten on PATH (emsdk_env.sh). Native targets never call emcc.
EMCC    ?= emcc
NODE    ?= $(or $(EMSDK_NODE),node)  # emsdk_env.sh sets it; its PATH holds a node/ directory
WEB_OPT ?= -O2
WEB_OUT ?= $(B)/web
WEB_WORK := $(WEB_OUT)-work
WEB_DEMO := $(WEB_WORK)/demo
# DOS path handling and the translated editor exceed Emscripten's default
# 64 KiB C stack. Checked builds exercise the same smoke test with 1 MiB.
WEB_FLAGS := $(WEB_OPT) -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 \
             -sSTACK_SIZE=1048576 \
             -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,node -sFORCE_FILESYSTEM \
             -sEXPORTED_RUNTIME_METHODS='["FS","ENV"]'
WEB_DEMO_INPUT := web/README.TXT web/BOOTLOGO.TXT web/GAMES/SPIRAL.BAS README.md asm/VC.ASM asm/VCOVL.ASM asm/LICENSE.TXT tools/web_demo.py tools/vz_defaults.py $(BASIC_GAME_FILES) $(B)/gwbasic/GWBASIC.EXE $(B)/bootlogo/LOGO.COM $(B)/rogue/ROGUE.EXE $(B)/rogue/OWLIC.TXT third_party/rogue/LICENSE.TXT third_party/pdcurses/README.md $(B)/vz/VZ.COM $(VZ_DATA) third_party/gwbasic/LICENSE third_party/bootlogo/LICENSE
WEB_DEMO_FILES := $(addprefix $(WEB_DEMO)/,README.TXT HISTORY.TXT SRC/VC.ASM SRC/VCOVL.ASM SRC/LICENSE.TXT GWBASIC.EXE GWBASIC.TXT BOOTLOGO.COM BOOTLOGO.TXT LOGOLIC.TXT GAMES/SPIRAL.BAS GAMES/ROGUE.EXE GAMES/ROGUELIC.TXT GAMES/PDCLIC.TXT GAMES/OWLIC.TXT VZ.COM VZ.DEF $(VZ_DEF_NAMES) VZLIC.TXT) $(patsubst $(B)/games/%,$(WEB_DEMO)/GAMES/%,$(BASIC_GAME_FILES))
WEB_ASSETS := web/index.html web/vc-web.js web/speaker.js web/graphics.js $(wildcard web/vendor/*)
WEB_COPIES := $(patsubst web/%,$(WEB_OUT)/%,$(WEB_ASSETS))

# Grouped targets keep both the demo preparation and the single emcc link safe
# under make -j.
$(WEB_DEMO_FILES) &: $(WEB_DEMO_INPUT)
	$(PY) tools/web_demo.py $(WEB_DEMO) $(B)/gwbasic/GWBASIC.EXE $(B)/games $(B)/bootlogo/LOGO.COM $(B)/rogue/ROGUE.EXE $(B)/vz/VZ.COM

$(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm &: $(RT_SRC) $(wildcard runtime/*.h) $(FONT_HEADERS) $(GEN_SRC) $(WEB_DEMO_FILES) Makefile
	@mkdir -p $(WEB_OUT)
	$(EMCC) $(WEB_FLAGS) \
	  -std=gnu11 -Iruntime $(RT_SRC) $(GEN_SRC) --embed-file $(WEB_DEMO)@/home/vc -o $(WEB_OUT)/vc.mjs

$(WEB_OUT)/%: web/%
	@mkdir -p $(dir $@)
	cp $< $@

# The page and its loader name every file with this build's hash, so a browser holding cached files
# from an older build never mixes the two. GitHub Pages caches each file for 10 minutes.
WEB_VERSIONED := $(WEB_OUT)/index.html $(WEB_OUT)/vc-web.js
$(WEB_VERSIONED): $(WEB_OUT)/%: web/% $(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm web/index.html web/vc-web.js web/speaker.js web/graphics.js
	@mkdir -p $(dir $@)
	v=$$(cat $(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm web/index.html web/vc-web.js web/speaker.js web/graphics.js | sha256sum | cut -c1-12); \
	  sed "s/__V__/$$v/g" $< > $@

web: $(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm $(WEB_COPIES)

test-web: web
	$(NODE) tests/web_assets.mjs $(WEB_OUT)
	$(NODE) tests/test_web_speaker.mjs
	$(NODE) tests/test_web_graphics.mjs
	$(NODE) tests/web_smoke.mjs $(WEB_OUT)/vc.mjs

.PHONY: web test-web
