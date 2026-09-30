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
