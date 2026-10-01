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

all: $(B)/vc

include tools/gwbasic.mk

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

gen: $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c $(B)/gen/gwbasic.c

test-translator: gen
	$(PY) -m pytest -q tests/test_translator_*.py

.PHONY: test-translator

# The terminal gate deliberately supplies its own Cpu and memory; CPU and DOS
# file-system implementations are independent work and must not be linked here.
$(B)/test_term: tests/test_term.c runtime/bios.c runtime/bios.h runtime/term.c runtime/term.h runtime/cpu.h runtime/hle.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Iruntime runtime/bios.c runtime/term.c runtime/cp866.c tests/test_term.c -o $@

test-term: $(B)/test_term
	./$(B)/test_term

$(B)/test_term_sanitize: tests/test_term.c runtime/bios.c runtime/bios.h runtime/term.c runtime/term.h runtime/cpu.h runtime/hle.h
	@mkdir -p $(B)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -Iruntime runtime/bios.c runtime/term.c runtime/cp866.c tests/test_term.c -o $@

test-term-sanitize: $(B)/test_term_sanitize
	./$(B)/test_term_sanitize

.PHONY: test-term test-term-sanitize

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

$(B)/test_machine: tests/test_machine.c runtime/rt.c runtime/cpu.c runtime/bios.c $(wildcard runtime/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime runtime/rt.c runtime/cpu.c runtime/bios.c tests/test_machine.c -o $@

test-machine: $(B)/test_machine
	./$(B)/test_machine

.PHONY: test-machine

$(B)/test_rt_process: tests/test_rt_process.c runtime/rt.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c runtime/bios.c $(wildcard runtime/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime tests/test_rt_process.c runtime/rt.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c runtime/bios.c -o $@

test-process: $(B)/test_rt_process
	./$(B)/test_rt_process

.PHONY: test-process

# ---- the native binary -----------------------------------------------------
RT_SRC := runtime/rt.c runtime/dos_core.c runtime/main.c runtime/cpu.c runtime/dos_fs.c \
          runtime/cp866.c runtime/bios.c runtime/term.c
GEN_SRC := $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c $(B)/gen/gwbasic.c $(B)/gen/files.c
GEN_OBJ := $(patsubst $(B)/gen/%.c,$(B)/obj/%.o,$(GEN_SRC))

$(B)/gen/files.c: $(B)/VC.COM $(B)/VC.OVL $(B)/gwbasic/GWBASIC.EXE data/VC.INI data/VC.EXT data/VCEDIT.EXT data/VC.HLP tools/embed.py
	@mkdir -p $(B)/gen
	$(PY) tools/embed.py $@ VC.COM=$(B)/VC.COM VC.OVL=$(B)/VC.OVL GWBASIC.EXE=$(B)/gwbasic/GWBASIC.EXE VC.INI=data/VC.INI VC.EXT=data/VC.EXT VCEDIT.EXT=data/VCEDIT.EXT VC.HLP=data/VC.HLP

$(B)/obj/%.o: $(B)/gen/%.c runtime/cpu.h runtime/image.h
	@mkdir -p $(B)/obj
	$(CC) $(CFLAGS) -Iruntime -c $< -o $@

$(B)/obj/files.o: runtime/rt.h

$(B)/vc: $(RT_SRC) $(wildcard runtime/*.h) $(GEN_OBJ)
	$(CC) $(CFLAGS) -std=gnu11 -Iruntime -o $@ $(RT_SRC) $(GEN_OBJ)

# The release binary: static and stripped, so it runs on any x86-64 Linux.
$(B)/vc-static: $(RT_SRC) $(wildcard runtime/*.h) $(GEN_OBJ)
	$(CC) -O2 -static -std=gnu11 -Iruntime -o $@ $(RT_SRC) $(GEN_OBJ)
	strip $@

BASIC_GAME_FILES := $(addprefix $(B)/games/,$(shell $(PY) tools/basic_games.py --names))
$(BASIC_GAME_FILES) &: tools/basic_games.py $(wildcard third_party/basic-computer-games/*/*.bas) third_party/basic-computer-games/LICENSE
	$(PY) tools/basic_games.py $(B)/games

games: $(BASIC_GAME_FILES)

test-e2e: $(B)/vc games
	$(PY) -m pytest -q tests/test_e2e.py tests/test_gwbasic_e2e.py tests/test_install_e2e.py tests/test_command_e2e.py

test-ini: $(B)/VC.OVL
	$(PY) -m pytest -q tests/test_setup_ini.py

test: test-translator test-fs test-exec test-machine test-process test-term test-ini test-e2e

clean:
	rm -rf $(B)

.PHONY: test-e2e games

# ---- the browser toy -------------------------------------------------------
# Needs Emscripten on PATH (emsdk_env.sh). Native targets never call emcc.
EMCC    ?= emcc
NODE    ?= $(or $(EMSDK_NODE),node)  # emsdk_env.sh sets it; its PATH holds a node/ directory
WEB_OPT ?= -O2
WEB_OUT ?= $(B)/web
WEB_WORK := $(WEB_OUT)-work
WEB_DEMO := $(WEB_WORK)/demo
WEB_FLAGS := $(WEB_OPT) -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 \
             -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,node -sFORCE_FILESYSTEM \
             -sEXPORTED_RUNTIME_METHODS='["FS","ENV"]'
WEB_DEMO_INPUT := web/README.TXT README.md asm/VC.ASM asm/VCOVL.ASM asm/LICENSE.TXT tools/web_demo.py $(BASIC_GAME_FILES) $(B)/gwbasic/GWBASIC.EXE third_party/gwbasic/LICENSE
WEB_DEMO_FILES := $(addprefix $(WEB_DEMO)/,README.TXT HISTORY.TXT SRC/VC.ASM SRC/VCOVL.ASM SRC/LICENSE.TXT GWBASIC.EXE GWBASIC.TXT) $(patsubst $(B)/games/%,$(WEB_DEMO)/GAMES/%,$(BASIC_GAME_FILES))
WEB_ASSETS := web/index.html web/vc-web.js web/speaker.js $(wildcard web/vendor/*)
WEB_COPIES := $(patsubst web/%,$(WEB_OUT)/%,$(WEB_ASSETS))

# Grouped targets keep both the demo preparation and the single emcc link safe
# under make -j.
$(WEB_DEMO_FILES) &: $(WEB_DEMO_INPUT)
	$(PY) tools/web_demo.py $(WEB_DEMO) $(B)/gwbasic/GWBASIC.EXE $(B)/games

$(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm &: $(RT_SRC) $(wildcard runtime/*.h) $(GEN_SRC) $(WEB_DEMO_FILES) Makefile
	@mkdir -p $(WEB_OUT)
	$(EMCC) $(WEB_FLAGS) \
	  -std=gnu11 -Iruntime $(RT_SRC) $(GEN_SRC) --embed-file $(WEB_DEMO)@/home/vc -o $(WEB_OUT)/vc.mjs

$(WEB_OUT)/%: web/%
	@mkdir -p $(dir $@)
	cp $< $@

web: $(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm $(WEB_COPIES)

test-web: web
	$(NODE) tests/test_web_speaker.mjs
	$(NODE) tests/web_smoke.mjs $(WEB_OUT)/vc.mjs

.PHONY: web test-web
