# VC 4.05 has its own tool compatibility mode; no vendored source edits.
VC405_IMAGES := $(B)/vc405/VC.COM $(B)/vc405/VCSETUP.COM
VC405_LISTINGS := $(addsuffix .lst,$(VC405_IMAGES))
VC405_GEN := $(B)/gen/vc405.c $(B)/gen/vcsetup405.c
VC405_INPUTS := $(wildcard asm405/*) tools/build_vc405.py tools/build_vc405_jwasm.py tools/vc405.mk
VC405_INPUTS += tools/jwasm/jwasm-a7c6e70.tar.gz tools/jwasm/vc405.patch
VC405_INPUTS += tools/vc405-options.inc $(wildcard tests/fixtures/vc405-tasm/*)

$(VC405_IMAGES) $(VC405_LISTINGS) &: $(VC405_INPUTS)
	$(PY) tools/build_vc405.py $(B)/vc405

vc405: $(VC405_IMAGES) $(VC405_LISTINGS)

$(B)/gen/vc405.c: $(B)/vc405/VC.COM $(B)/vc405/VC.COM.lst $(wildcard translator/*.py)
	@mkdir -p $(B)/gen
	$(PY) -m translator $(B)/vc405/VC.COM $(B)/vc405/VC.COM.lst --format vc405 --name VC405.COM --symbol image_vc405 -o $@

$(B)/gen/vcsetup405.c: $(B)/vc405/VCSETUP.COM $(B)/vc405/VCSETUP.COM.lst $(wildcard translator/*.py)
	@mkdir -p $(B)/gen
	$(PY) -m translator $(B)/vc405/VCSETUP.COM $(B)/vc405/VCSETUP.COM.lst --format vc405 --name VCSETUP.COM --symbol image_vcsetup405 -o $@

test-vc405-build: vc405
	$(PY) -m pytest -q tests/test_vc405_build.py

.PHONY: vc405 test-vc405-build
