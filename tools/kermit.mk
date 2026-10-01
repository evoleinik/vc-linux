# MS-DOS Kermit sources stay byte-for-byte identical to the upstream archive.
KERMIT_MODULES := msscmd msscom mssfil mssker mssrcv mssscp msssen mssser mssset msssho msster msuibm msgibm msxibm msyibm mszibm
KERMIT_LISTINGS := $(addprefix $(B)/kermit/,$(addsuffix .lst,$(KERMIT_MODULES)))
KERMIT_INPUT := $(wildcard third_party/mskermit/*) tools/build_kermit.py tools/kermit.mk tools/kermit-options.inc tools/build_kermit_jwasm.py tools/jwasm/kermit.patch tools/jwasm/jwasm-a7c6e70.tar.gz
KERMIT_INPUT += translator/listing.py translator/omf.py translator/layout.py

$(B)/kermit/KERMIT.EXE $(B)/kermit/KERMIT.MAP $(KERMIT_LISTINGS) &: $(KERMIT_INPUT)
	$(PY) tools/build_kermit.py $(B)/kermit

kermit: $(B)/kermit/KERMIT.EXE $(B)/kermit/KERMIT.MAP $(KERMIT_LISTINGS)

test-kermit-build: kermit
	$(PY) -m pytest -q tests/test_kermit_build.py

$(B)/gen/kermit.c: $(B)/kermit/KERMIT.EXE $(B)/kermit/KERMIT.MAP $(KERMIT_LISTINGS) $(wildcard translator/*.py)
	$(PY) -m translator $(B)/kermit/KERMIT.EXE $(KERMIT_LISTINGS) --map $(B)/kermit/KERMIT.MAP --name KERMIT.EXE --symbol image_kermit -o $@

.PHONY: kermit test-kermit-build
