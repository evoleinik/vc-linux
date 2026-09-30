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

gen: $(B)/gen/vc_com.c $(B)/gen/vc_ovl.c

test-translator: gen
	$(PY) -m pytest -q tests/test_translator_*.py

.PHONY: test-translator
