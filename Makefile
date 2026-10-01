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
ZIG     ?= $(B)/zig/zig
CFLAGS  ?= -O1 -g -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
ASM     := $(wildcard asm/*.ASM asm/*.INC)
FONT_HEADERS := $(wildcard third_party/font8x8/*.h)

all: $(B)/vc

include tools/gwbasic.mk
include tools/bootlogo.mk
include tools/rogue.mk
include tools/vz.mk
include tools/kermit.mk

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
gen: $(B)/gen/kermit.c

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

$(B)/test_machine: tests/test_machine.c runtime/rt.c runtime/cpu.c runtime/bios.c runtime/modem.c $(wildcard runtime/*.h) $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime runtime/rt.c runtime/cpu.c runtime/bios.c runtime/modem.c tests/test_machine.c -o $@

test-machine: $(B)/test_machine
	./$(B)/test_machine

.PHONY: test-machine

$(B)/test_rt_process: tests/test_rt_process.c runtime/rt.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c runtime/bios.c runtime/modem.c $(wildcard runtime/*.h) $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime tests/test_rt_process.c runtime/rt.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c runtime/bios.c runtime/modem.c -o $@

test-process: $(B)/test_rt_process
	./$(B)/test_rt_process

.PHONY: test-process

# ---- the native binary -----------------------------------------------------
RT_SRC := runtime/rt.c runtime/dos_core.c runtime/main.c runtime/cpu.c runtime/dos_fs.c \
          runtime/cp866.c runtime/bios.c runtime/term.c runtime/modem.c runtime/modem_transport.c
GEN_SRC := $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c $(B)/gen/gwbasic.c $(B)/gen/bootlogo.c $(B)/gen/gwbasic_graphics.c $(B)/gen/rogue.c $(B)/gen/vz.c $(B)/gen/kermit.c $(B)/gen/files.c
GEN_OBJ := $(patsubst $(B)/gen/%.c,$(B)/obj/%.o,$(GEN_SRC))
NATIVE_RT_SRC := $(RT_SRC) runtime/door.c runtime/door_confinement.c
NATIVE_GEN_SRC := $(GEN_SRC) $(B)/gen/door_demo.c
NATIVE_GEN_OBJ := $(GEN_OBJ) $(B)/obj/door_demo.o

$(B)/gen/files.c: $(B)/VC.COM $(B)/VC.OVL $(B)/gwbasic/GWBASIC.EXE $(B)/bootlogo/LOGO.COM $(B)/rogue/ROGUE.EXE $(B)/rogue/OWLIC.TXT third_party/rogue/LICENSE.TXT third_party/pdcurses/README.md $(B)/vz/VZ.COM $(VZ_DATA) data/VC.INI data/VC.EXT data/VCEDIT.EXT data/VC.HLP tools/embed.py Makefile

KERMIT_DATA := data/BBS.TAK data/KERMIT.TXT third_party/mskermit/LICENSE
KERMIT_EMBED := KERMIT.EXE=$(B)/kermit/KERMIT.EXE BBS.TAK=data/BBS.TAK KERMIT.TXT=data/KERMIT.TXT KERMLIC.TXT=third_party/mskermit/LICENSE

$(B)/gen/files.c: $(B)/kermit/KERMIT.EXE $(KERMIT_DATA) translator/image.py
	@mkdir -p $(B)/gen
	$(PY) tools/embed.py $@ VC.COM=$(B)/VC.COM VC.OVL=$(B)/VC.OVL GWBASIC.EXE=$(B)/gwbasic/GWBASIC.EXE BOOTLOGO.COM=$(B)/bootlogo/LOGO.COM ROGUE.EXE=$(B)/rogue/ROGUE.EXE ROGUELIC.TXT=third_party/rogue/LICENSE.TXT PDCLIC.TXT=third_party/pdcurses/README.md OWLIC.TXT=$(B)/rogue/OWLIC.TXT $(VZ_EMBED) $(KERMIT_EMBED) VC.INI=data/VC.INI VC.EXT=data/VC.EXT VCEDIT.EXT=data/VCEDIT.EXT VC.HLP=data/VC.HLP

$(B)/obj/%.o: $(B)/gen/%.c runtime/cpu.h runtime/image.h
	@mkdir -p $(B)/obj
	$(CC) $(CFLAGS) -Iruntime -c $< -o $@

$(B)/obj/files.o: runtime/rt.h
$(B)/obj/door_demo.o: runtime/door_demo.h runtime/rt.h

$(B)/vc: $(NATIVE_RT_SRC) $(wildcard runtime/*.h) $(FONT_HEADERS) $(NATIVE_GEN_OBJ)
	$(CC) $(CFLAGS) -std=gnu11 -pthread -Iruntime -o $@ $(NATIVE_RT_SRC) $(NATIVE_GEN_OBJ)

# Supplemental no-network pty gate. This is never a production fallback and
# does not replace the required real TCP loopback test.
$(B)/vc-pipe-modem: $(NATIVE_RT_SRC) tests/serial_pipe_transport.c $(wildcard runtime/*.h) $(FONT_HEADERS) $(NATIVE_GEN_OBJ)
	$(CC) $(CFLAGS) -std=gnu11 -Iruntime -o $@ $(filter-out runtime/modem_transport.c,$(NATIVE_RT_SRC)) tests/serial_pipe_transport.c $(NATIVE_GEN_OBJ)

# The release binary: static and stripped, so it runs on any x86-64 Linux.
$(B)/vc-static: $(NATIVE_RT_SRC) $(wildcard runtime/*.h) $(FONT_HEADERS) $(NATIVE_GEN_OBJ)
	$(CC) -O2 -static -std=gnu11 -pthread -Iruntime -o $@ $(NATIVE_RT_SRC) $(NATIVE_GEN_OBJ)
	strip $@

# Zig 0.16.0 cross-compiles the same unedited translations and native runtime.
# Keep target objects and caches separate from gcc and Emscripten artifacts.
DOOR_CFLAGS ?= -O2 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
DOOR_ZIG_ENV := ZIG_GLOBAL_CACHE_DIR=$(abspath $(B)/zig-cache/global) ZIG_LOCAL_CACHE_DIR=$(abspath $(B)/zig-cache/local)
DOOR_CC = $(DOOR_ZIG_ENV) $(ZIG) cc -target aarch64-linux-musl -static
DOOR_GEN_OBJ := $(patsubst $(B)/gen/%.c,$(B)/obj-door-aarch64/gen/%.o,$(NATIVE_GEN_SRC))
DOOR_RT_OBJ := $(patsubst runtime/%.c,$(B)/obj-door-aarch64/runtime/%.o,$(NATIVE_RT_SRC))

$(B)/obj-door-aarch64/gen/%.o: $(B)/gen/%.c $(wildcard runtime/*.h) Makefile
	@mkdir -p $(dir $@)
	$(DOOR_CC) $(DOOR_CFLAGS) -std=gnu11 -Iruntime -c $< -o $@

$(B)/obj-door-aarch64/runtime/%.o: runtime/%.c $(wildcard runtime/*.h) $(FONT_HEADERS) Makefile
	@mkdir -p $(dir $@)
	$(DOOR_CC) $(DOOR_CFLAGS) -std=gnu11 -pthread -Iruntime -c $< -o $@

$(B)/vc-door-aarch64: $(DOOR_GEN_OBJ) $(DOOR_RT_OBJ)
	$(DOOR_CC) -pthread -s -o $@ $(DOOR_GEN_OBJ) $(DOOR_RT_OBJ)

BASIC_GAME_FILES := $(addprefix $(B)/games/,$(shell $(PY) tools/basic_games.py --names))
$(BASIC_GAME_FILES) &: tools/basic_games.py $(wildcard third_party/basic-computer-games/*/*.bas) third_party/basic-computer-games/LICENSE
	$(PY) tools/basic_games.py $(B)/games

games: $(BASIC_GAME_FILES)

test-e2e: $(B)/vc $(B)/vc-pipe-modem games
	$(PY) -m pytest -q tests/test_e2e.py tests/test_gwbasic_e2e.py tests/test_install_e2e.py tests/test_command_e2e.py tests/test_logo_e2e.py tests/test_rogue_e2e.py tests/test_vz_e2e.py tests/test_kermit_e2e.py

test-ini: $(B)/VC.OVL
	$(PY) -m pytest -q tests/test_setup_ini.py

test-rogue-build: rogue
	$(PY) -m pytest -q tests/test_rogue_build.py

test: test-translator test-fs test-exec test-machine test-process test-term test-cga test-ini test-rogue-build test-vz-build test-e2e
test: test-kermit-build test-modem test-serial-machine test-modem-transport
test: test-modem-transport-unit
test: test-embed
test: test-door-packaging

test-embed:
	$(PY) -m pytest -q tests/test_embed.py

.PHONY: test-embed

test-door-packaging: $(B)/gen/door_demo.c
	$(PY) -m pytest -q tests/test_door_packaging.py

# The deep-tree case builds its tree through the real DOS rename, so the DOS
# file layer is linked; the linker drops the unused DOS/terminal entry points.
$(B)/test_door_cleanup: tests/test_door_cleanup.c runtime/door.c runtime/dos_fs.c runtime/cp866.c $(wildcard runtime/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -ffunction-sections -fdata-sections -Wl,--gc-sections -Iruntime $< runtime/dos_fs.c runtime/cp866.c -o $@

# Test-only native syscall hooks behind VC_DOOR_TEST_HOOKS. Never released.
$(B)/vc-door-confinement-test: $(NATIVE_RT_SRC) tests/test_door_confinement_hook.c $(wildcard runtime/*.h) $(FONT_HEADERS) $(NATIVE_GEN_OBJ)
	$(CC) $(CFLAGS) -std=gnu11 -pthread -DVC_DOOR_TEST_HOOKS -Iruntime -o $@ $(NATIVE_RT_SRC) tests/test_door_confinement_hook.c $(NATIVE_GEN_OBJ)

# Production DOS, BIOS, loader and modem code under ASan and UBSan; any report
# aborts. The program stubs are the only fakes (tests/test_door_memory.c).
DOOR_MEMORY_SRC := runtime/dos_core.c runtime/dos_fs.c runtime/bios.c runtime/modem.c runtime/cpu.c runtime/cp866.c
$(B)/test_door_memory_sanitize: tests/test_door_memory.c runtime/rt.c $(DOOR_MEMORY_SRC) $(wildcard runtime/*.h) $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) -std=gnu11 -O1 -g -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -U_FORTIFY_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Iruntime tests/test_door_memory.c $(DOOR_MEMORY_SRC) -o $@

test-door-memory: $(B)/test_door_memory_sanitize
	for t in --loader --overlap --cycle --merge-wrap --fuzz; do \
		ASAN_OPTIONS=halt_on_error=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(B)/test_door_memory_sanitize $$t || exit 1; done

test-door-confinement: $(B)/vc-door-confinement-test
	$(PY) -m pytest -q tests/test_door_confinement.py

test-door: $(B)/vc $(B)/test_door_cleanup
	./$(B)/test_door_cleanup
	$(PY) -m pytest -q tests/test_door_e2e.py

test: test-door test-door-confinement test-door-memory

.PHONY: test-door-packaging test-door test-door-confinement test-door-memory

$(B)/test_modem: runtime/modem.c runtime/modem.h tests/test_modem.c tests/fixtures/enigma-connect-2026-10-02.bin
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime runtime/modem.c tests/test_modem.c -o $@

test-modem: $(B)/test_modem
	./$(B)/test_modem

$(B)/test_serial_machine: tests/test_serial_machine.c runtime/rt.c runtime/cpu.c runtime/bios.c runtime/modem.c $(wildcard runtime/*.h) $(FONT_HEADERS)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -Iruntime runtime/rt.c runtime/cpu.c runtime/bios.c runtime/modem.c tests/test_serial_machine.c -o $@

test-serial-machine: $(B)/test_serial_machine
	./$(B)/test_serial_machine

.PHONY: test-modem test-serial-machine

$(B)/test_modem_transport: runtime/modem_transport.c runtime/modem_transport.h runtime/modem.h tests/test_modem_transport.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -pthread -Wl,--wrap=getaddrinfo -Iruntime runtime/modem_transport.c tests/test_modem_transport.c -o $@

test-modem-transport: $(B)/test_modem_transport
	./$(B)/test_modem_transport

.PHONY: test-modem-transport

MODEM_TEST_WRAPS := socket connect poll __poll_chk getsockopt recv __recv_chk send close getaddrinfo freeaddrinfo clock_gettime
$(B)/test_modem_transport_unit: runtime/modem_transport.c runtime/modem_transport.h runtime/modem.h tests/test_modem_transport_unit.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -std=gnu11 -Wextra -pthread $(foreach f,$(MODEM_TEST_WRAPS),-Wl,--wrap=$(f)) -Iruntime runtime/modem_transport.c tests/test_modem_transport_unit.c -o $@

test-modem-transport-unit: $(B)/test_modem_transport_unit
	./$(B)/test_modem_transport_unit

.PHONY: test-modem-transport-unit

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
# PIC expands inlined translation helpers. Keeping them out of line makes
# -O2 smaller than -Os/-Oz here without editing the generated C. Side modules
# retain their ordinary -O2 build. Measurements are in the mobile plan.
# 64 MiB covers the guard's 2N + 64 KiB for all five modules in any load order,
# with headroom. test-web prints that bound and fails if memory would grow.
WEB_FLAGS := $(WEB_OPT) -flto -fno-inline-functions -sMALLOC=emmalloc -sMAIN_MODULE=2 -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 \
             -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=1 -sABORTING_MALLOC=0 \
             -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,node -sFORCE_FILESYSTEM \
             -sEXPORTED_RUNTIME_METHODS='["FS","ENV"]' \
             -sEXPORTED_FUNCTIONS='["_main","_cpu","_mem","_cpu_int","_flags_get","_flags_set","_port_in8","_port_out8","_port_in16","_port_out16","_rt_budget","_rt_code_delta","_rt_fault","_rt_halted","_sbrk"]'
# Retain only the host ABI the unedited side modules import. MAIN_MODULE=1
# would keep all of libc; linking side files into the main link would make
# them eager dylink dependencies. tests/web_modules.mjs guards both choices.
WEB_MAIN_SRC := $(RT_SRC) runtime/web_programs.c runtime/embed_lzma.c $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c $(B)/gen/files.c
WEB_PROGRAMS := gwbasic bootlogo rogue vz kermit
# Unversioned side binaries are build inputs only, never published. Their
# content-derived generation hash is independent of the page's ?v= hash:
# putting the latter into main's strings would create a circular hash.
WEB_MODULES := $(addprefix $(WEB_WORK)/,$(addsuffix .wasm,$(WEB_PROGRAMS)))
WEB_MODULE_HEADER := $(WEB_WORK)/web_program_names.h
WEB_BINARIES := $(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm $(WEB_MODULES)
WEB_SIDE_FLAGS := $(WEB_OPT) -sSIDE_MODULE=2 -std=gnu11 -Iruntime
WEB_DEMO_INPUT := web/README.TXT web/README-RU.TXT web/BOOTLOGO.TXT web/GAMES/SPIRAL.BAS README.md asm/VC.ASM asm/VCOVL.ASM asm/LICENSE.TXT tools/web_demo.py tools/vz_defaults.py $(BASIC_GAME_FILES) $(B)/gwbasic/GWBASIC.EXE $(B)/bootlogo/LOGO.COM $(B)/rogue/ROGUE.EXE $(B)/rogue/OWLIC.TXT third_party/rogue/LICENSE.TXT third_party/pdcurses/README.md $(B)/vz/VZ.COM $(VZ_DATA) third_party/gwbasic/LICENSE third_party/bootlogo/LICENSE
WEB_DEMO_INPUT += $(B)/kermit/KERMIT.EXE $(KERMIT_DATA)

# Native packaging calls the very same generator and uses the browser inputs.
# It never invokes emcc and never embeds a separately curated demo file list.
$(B)/gen/door_demo.c: $(WEB_DEMO_INPUT) tools/door_demo.py tools/embed.py tools/vcini.py runtime/door_demo.h data/VC.INI $(B)/gen/VC.OVL.lst
	@mkdir -p $(B)/gen
	$(PY) tools/door_demo.py $@ $(B)/gwbasic/GWBASIC.EXE $(B)/games $(B)/bootlogo/LOGO.COM $(B)/rogue/ROGUE.EXE $(B)/vz/VZ.COM $(B)/kermit/KERMIT.EXE

WEB_DEMO_FILES := $(addprefix $(WEB_DEMO)/,README.TXT ПРОЧТИ.TXT HISTORY.TXT SRC/VC.ASM SRC/VCOVL.ASM SRC/LICENSE.TXT GWBASIC.EXE GWBASIC.TXT BOOTLOGO.COM BOOTLOGO.TXT LOGOLIC.TXT GAMES/SPIRAL.BAS GAMES/ROGUE.EXE GAMES/ROGUELIC.TXT GAMES/PDCLIC.TXT GAMES/OWLIC.TXT VZ.COM VZ.DEF $(VZ_DEF_NAMES) VZLIC.TXT) $(patsubst $(B)/games/%,$(WEB_DEMO)/GAMES/%,$(BASIC_GAME_FILES))
WEB_DEMO_FILES += $(addprefix $(WEB_DEMO)/,KERMIT.EXE BBS.TAK KERMIT.TXT KERMLIC.TXT)
# main.c already installs these exact files from gen/files.c. Do not embed
# a second copy in MEMFS's startup package. Keep the complete demo directory
# for the packaging/content gates, including the byte-matched executables.
WEB_INSTALLED := GWBASIC.EXE BOOTLOGO.COM GAMES/ROGUE.EXE GAMES/ROGUELIC.TXT GAMES/PDCLIC.TXT GAMES/OWLIC.TXT VZ.COM VZ.DEF $(VZ_DEF_NAMES) VZLIC.TXT
WEB_INSTALLED += KERMIT.EXE BBS.TAK KERMIT.TXT KERMLIC.TXT
WEB_PACKED_FILES := $(filter-out $(addprefix $(WEB_DEMO)/,$(WEB_INSTALLED)),$(WEB_DEMO_FILES))
# Emscripten 4.0.2's file_packager emits invalid assembler symbols for a
# Cyrillic destination. Use an ASCII staging name, renamed before DOS starts.
WEB_EMBED_FLAGS := $(foreach f,$(WEB_PACKED_FILES),--embed-file $(f)@/home/vc/$(subst ПРОЧТИ.TXT,READMERU.TXT,$(patsubst $(WEB_DEMO)/%,%,$(f))))
WEB_ASSETS := web/index.html web/vc-web.js web/vc-layout.js web/vc-source.js web/vc-keypad.js web/vc-language.js web/speaker.js web/graphics.js web/modem.js $(wildcard web/vendor/*)
WEB_COPIES := $(patsubst web/%,$(WEB_OUT)/%,$(WEB_ASSETS))
WEB_SOURCE_INDEX := $(WEB_WORK)/source-index-name.txt
WEB_SOURCE_INPUTS := $(B)/VC.COM $(B)/VC.OVL $(B)/gen/VC.COM.lst $(B)/gen/VC.OVL.lst \
                    $(B)/gwbasic/GWBASIC.EXE $(B)/gwbasic/GWBASIC.MAP $(GWB_LISTINGS) \
                    $(B)/bootlogo/LOGO.COM $(B)/bootlogo/LOGO.lst \
                    $(B)/rogue/ROGUE.EXE $(B)/rogue/ROGUE.MAP \
                    $(B)/vz/VZ.COM $(B)/vz/VZ.MAP $(VZ_LISTINGS) \
                    $(wildcard asm/* third_party/gwbasic/* third_party/vzeditor/SRC/*) \
                    third_party/bootlogo/bootlogo.asm tools/source_maps.py tools/web_modules.py \
                    tools/build_gwbasic.py tools/build_vz.py $(wildcard translator/*.py)

# Source data is never embedded in the main wasm or eagerly imported by the
# page. Every map and original-text payload, including its index, is immutable.
$(WEB_SOURCE_INDEX): $(WEB_SOURCE_INPUTS)
	$(PY) tools/source_maps.py --build $(B) --out $(WEB_OUT) --work $(WEB_WORK)
	@touch $@

# Grouped targets keep both the demo preparation and the single emcc link safe
# under make -j.
$(WEB_DEMO_FILES) &: $(WEB_DEMO_INPUT)
	$(PY) tools/web_demo.py $(WEB_DEMO) $(B)/gwbasic/GWBASIC.EXE $(B)/games $(B)/bootlogo/LOGO.COM $(B)/rogue/ROGUE.EXE $(B)/vz/VZ.COM $(B)/kermit/KERMIT.EXE

$(WEB_MODULE_HEADER): $(WEB_MODULES) tools/web_modules.py
	$(PY) tools/web_modules.py $(WEB_WORK) $(WEB_OUT)

$(WEB_OUT)/vc.mjs $(WEB_OUT)/vc.wasm &: $(WEB_MAIN_SRC) $(wildcard runtime/*.h) $(FONT_HEADERS) $(WEB_DEMO_FILES) $(WEB_MODULE_HEADER) Makefile
	@mkdir -p $(WEB_OUT)
	$(EMCC) $(WEB_FLAGS) \
	  -std=gnu11 -Iruntime -I$(WEB_WORK) $(WEB_MAIN_SRC) $(WEB_EMBED_FLAGS) -o $(WEB_OUT)/vc.mjs

# Translated code never suspends. BASIC's source-proved graphics supplement
# travels with its image; VC.COM and VC.OVL stay in the main wasm above.
$(WEB_WORK)/gwbasic.wasm: $(B)/gen/gwbasic.c $(B)/gen/gwbasic_graphics.c runtime/cpu.h runtime/image.h Makefile
	@mkdir -p $(WEB_WORK)
	$(EMCC) $(WEB_SIDE_FLAGS) $(B)/gen/gwbasic.c $(B)/gen/gwbasic_graphics.c -sEXPORTED_FUNCTIONS='["_image_gwbasic","_run_gwbasic_graphics"]' -o $@

$(WEB_WORK)/bootlogo.wasm: $(B)/gen/bootlogo.c runtime/cpu.h runtime/image.h Makefile
	@mkdir -p $(WEB_WORK)
	$(EMCC) $(WEB_SIDE_FLAGS) $< -sEXPORTED_FUNCTIONS='["_image_bootlogo"]' -o $@

$(WEB_WORK)/rogue.wasm: $(B)/gen/rogue.c runtime/cpu.h runtime/image.h Makefile
	@mkdir -p $(WEB_WORK)
	$(EMCC) $(WEB_SIDE_FLAGS) $< -sEXPORTED_FUNCTIONS='["_image_rogue"]' -o $@

$(WEB_WORK)/vz.wasm: $(B)/gen/vz.c runtime/cpu.h runtime/image.h Makefile
	@mkdir -p $(WEB_WORK)
	$(EMCC) $(WEB_SIDE_FLAGS) $< -sEXPORTED_FUNCTIONS='["_image_vz"]' -o $@

$(WEB_WORK)/kermit.wasm: $(B)/gen/kermit.c runtime/cpu.h runtime/image.h Makefile
	@mkdir -p $(WEB_WORK)
	$(EMCC) $(WEB_SIDE_FLAGS) $< -sEXPORTED_FUNCTIONS='["_image_kermit"]' -o $@

$(WEB_OUT)/%: web/%
	@mkdir -p $(dir $@)
	cp $< $@

# Startup assets keep today's ?v= stamp. Lazy modules instead carry their
# side-generation hash in the filename, which Pages cannot ignore on deploy.
WEB_VERSIONED := $(WEB_OUT)/index.html $(WEB_OUT)/vc-web.js
$(WEB_VERSIONED): $(WEB_OUT)/%: web/% $(WEB_BINARIES) $(WEB_ASSETS) $(WEB_SOURCE_INDEX)
	@mkdir -p $(dir $@)
	v=$$(cat $(WEB_BINARIES) $(WEB_ASSETS) $(WEB_SOURCE_INDEX) | sha256sum | cut -c1-12); \
	  source_index=$$(cat $(WEB_SOURCE_INDEX)); \
	  sed "s/__V__/$$v/g; s/__SOURCE_INDEX__/$$source_index/g" $< > $@

web: $(WEB_BINARIES) $(WEB_COPIES)
	@$(PY) tools/web_modules.py $(WEB_WORK) $(WEB_OUT)
	@$(PY) tools/source_maps.py --restore --out $(WEB_OUT) --work $(WEB_WORK)

$(WEB_WORK)/source-runtime.mjs $(WEB_WORK)/source-runtime.wasm &: tests/source_runtime.c runtime/rt.c $(wildcard runtime/*.h)
	@mkdir -p $(WEB_WORK)
	$(EMCC) -O2 -sENVIRONMENT=node -sMODULARIZE -sEXPORT_ES6 -sASSERTIONS \
	  -sSTACK_SIZE=1048576 -Iruntime tests/source_runtime.c -o $(WEB_WORK)/source-runtime.mjs

test-web: web $(WEB_WORK)/source-runtime.mjs
	$(NODE) tests/web_assets.mjs $(WEB_OUT)
	$(NODE) tests/web_size.mjs $(WEB_OUT)
	$(NODE) tests/test_source_maps.mjs $(WEB_OUT)
	$(NODE) tests/test_web_source.mjs $(WEB_OUT)
	$(NODE) tests/test_web_layout.mjs $(WEB_OUT)
	$(NODE) tests/test_web_source_wiring.mjs $(WEB_OUT)
	$(NODE) tests/source_runtime.mjs $(WEB_WORK)/source-runtime.mjs
	$(NODE) tests/web_modules.mjs $(WEB_OUT)
	$(NODE) tests/test_web_keypad.mjs $(WEB_OUT)
	$(NODE) tests/web_language.mjs $(WEB_DEMO)
	$(NODE) tests/test_web_speaker.mjs
	$(NODE) tests/test_web_graphics.mjs
	$(NODE) tests/test_web_modem.mjs
	$(NODE) tests/web_smoke.mjs $(WEB_OUT)/vc.mjs
	$(NODE) tests/web_smoke.mjs $(WEB_OUT)/vc.mjs --fetch-failure
	$(NODE) tests/web_smoke.mjs $(WEB_OUT)/vc.mjs --fetch-timeout
	$(NODE) tests/web_smoke.mjs $(WEB_OUT)/vc.mjs --memory-limit

.PHONY: web test-web
