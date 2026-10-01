# Original Microsoft sources remain byte-for-byte unchanged; all dialect fixes
# live in the isolated tool and its command-line options.
MSDOS_PROGRAMS := command edlin debug find more sort fc
MSDOS_command_FILE := COMMAND.COM
MSDOS_edlin_FILE := EDLIN.COM
MSDOS_debug_FILE := DEBUG.COM
MSDOS_find_FILE := FIND.EXE
MSDOS_more_FILE := MORE.COM
MSDOS_sort_FILE := SORT.EXE
MSDOS_fc_FILE := FC.EXE
MSDOS_command_MODULES := command rucode rdata init uinit tcode tcode2 tcode3 tcode4 tcode5 tucode copy copyproc cparse tdata tspc
MSDOS_edlin_MODULES := edlin edlproc edlmes
MSDOS_debug_MODULES := debug debcom1 debcom2 debasm debuasm debconst debmes debdata
MSDOS_find_MODULES := find findmes
MSDOS_more_MODULES := more moremes
MSDOS_sort_MODULES := sort sortmes
MSDOS_fc_MODULES := fc fcmes
MSDOS_MODULES := $(foreach p,$(MSDOS_PROGRAMS),$(MSDOS_$(p)_MODULES))
MSDOS_LISTINGS := $(addprefix $(B)/msdos2/,$(addsuffix .lst,$(MSDOS_MODULES)))
MSDOS_OBJECTS := $(addprefix $(B)/msdos2/,$(addsuffix .obj,$(MSDOS_MODULES)))
MSDOS_IMAGES := $(foreach p,$(MSDOS_PROGRAMS),$(B)/msdos2/$(MSDOS_$(p)_FILE))
MSDOS_MAPS := $(foreach p,$(MSDOS_PROGRAMS),$(B)/msdos2/$(basename $(MSDOS_$(p)_FILE)).MAP)
MSDOS_INPUT := $(wildcard third_party/msdos2/* third_party/msdos2/source/* third_party/msdos2/bin/*)
MSDOS_INPUT += tools/build_msdos.py tools/build_msdos_jwasm.py tools/msdos-options.inc tools/msdos.mk
MSDOS_INPUT += tools/jwasm/jwasm-a7c6e70.tar.gz tools/jwasm/kermit.patch tools/jwasm/msdos.patch tools/jwlink/jwlink
MSDOS_INPUT += translator/listing.py translator/omf.py translator/layout.py

$(MSDOS_IMAGES) $(MSDOS_MAPS) $(MSDOS_LISTINGS) $(MSDOS_OBJECTS) $(B)/msdos2/COMMAND.MZ $(B)/msdos2/SORT.MZ $(B)/msdos2/comparison.json &: $(MSDOS_INPUT)
	$(PY) tools/build_msdos.py $(B)/msdos2

msdos2: $(MSDOS_IMAGES) $(MSDOS_MAPS) $(MSDOS_LISTINGS) $(B)/msdos2/COMMAND.MZ $(B)/msdos2/SORT.MZ $(B)/msdos2/comparison.json

test-msdos2-build: msdos2
	$(PY) -m pytest -q tests/test_msdos_build.py

define MSDOS_TRANSLATE
$(B)/gen/$(1).c: $(B)/msdos2/$(MSDOS_$(1)_FILE) $(B)/msdos2/$(basename $(MSDOS_$(1)_FILE)).MAP $(addprefix $(B)/msdos2/,$(addsuffix .lst,$(MSDOS_$(1)_MODULES))) $(wildcard translator/*.py)
	$(PY) -m translator $(B)/msdos2/$(MSDOS_$(1)_FILE) $(addprefix $(B)/msdos2/,$(addsuffix .lst,$(MSDOS_$(1)_MODULES))) --map $(B)/msdos2/$(basename $(MSDOS_$(1)_FILE)).MAP --format msdos --name $(MSDOS_$(1)_FILE) --symbol image_$(1) -o $$@
endef
$(foreach p,$(MSDOS_PROGRAMS),$(eval $(call MSDOS_TRANSLATE,$(p))))

.PHONY: msdos2 test-msdos2-build
