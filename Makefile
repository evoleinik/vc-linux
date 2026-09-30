# vc-linux build.
#   make images   assemble VC.COM and VC.OVL from asm/ with JWasm, plus listings
#   make gen      translate both images to C (build/gen/)
#   make          build the native binary build/vc
#   make test     run every test suite

JWASM   := tools/jwasm/jwasm
JFLAGS  := -q -Zg -Zne -DOFFICIAL
PY      := .venv/bin/python
B       := build
CC      ?= gcc
CFLAGS  ?= -O1 -g -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
ASM     := $(wildcard asm/*.ASM asm/*.INC)

all: $(B)/vc

images: $(B)/VC.COM $(B)/VC.OVL

$(B)/VC.COM: $(ASM)
	@mkdir -p $(B)
	cd asm && ../$(JWASM) $(JFLAGS) -bin -Fl=../$(B)/VC.COM.lst -Fo ../$(B)/VC.COM VC.ASM

$(B)/VC.OVL: $(ASM)
	@mkdir -p $(B)
	cd asm && ../$(JWASM) $(JFLAGS) -mz -Fl=../$(B)/VC.OVL.lst -Fo ../$(B)/VC.OVL VCOVL.ASM

.PHONY: all images gen test clean

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

# ---- the native binary -----------------------------------------------------
RT_SRC := runtime/rt.c runtime/dos_core.c runtime/main.c runtime/cpu.c runtime/dos_fs.c \
          runtime/cp866.c runtime/bios.c runtime/term.c
GEN_SRC := $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c $(B)/gen/files.c

$(B)/gen/files.c: $(B)/VC.COM $(B)/VC.OVL data/VC.INI data/VC.EXT data/VC.HLP tools/embed.py
	@mkdir -p $(B)/gen
	$(PY) tools/embed.py $@ VC.COM=$(B)/VC.COM VC.OVL=$(B)/VC.OVL VC.INI=data/VC.INI VC.EXT=data/VC.EXT VC.HLP=data/VC.HLP

$(B)/obj/%.o: $(B)/gen/%.c runtime/cpu.h runtime/image.h runtime/rt.h
	@mkdir -p $(B)/obj
	$(CC) $(CFLAGS) -Iruntime -c $< -o $@

$(B)/vc: $(RT_SRC) $(wildcard runtime/*.h) $(B)/obj/vc_com.o $(B)/obj/vc_ovl.o $(B)/obj/files.o
	$(CC) $(CFLAGS) -std=gnu11 -Iruntime -o $@ $(RT_SRC) $(B)/obj/vc_com.o $(B)/obj/vc_ovl.o $(B)/obj/files.o

test-e2e: $(B)/vc
	$(PY) -m pytest -q tests/test_e2e.py

test: test-translator test-fs test-term test-e2e

clean:
	rm -rf $(B)

.PHONY: test-e2e
