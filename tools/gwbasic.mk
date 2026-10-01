# Included by the main Makefile after B/PY have been defined.
GWB_MODULES := $(shell $(PY) tools/build_gwbasic.py --modules)
GWB_LISTINGS := $(addprefix $(B)/gwbasic/,$(addsuffix .lst,$(GWB_MODULES)))
GWB_INPUT := $(wildcard third_party/gwbasic/*) tools/build_gwbasic.py tools/gwbasic.mk

$(B)/gwbasic/GWBASIC.EXE $(B)/gwbasic/GWBASIC.MAP $(GWB_LISTINGS) &: $(GWB_INPUT)
	$(PY) tools/build_gwbasic.py $(B)/gwbasic

gwbasic: $(B)/gwbasic/GWBASIC.EXE $(B)/gwbasic/GWBASIC.MAP $(GWB_LISTINGS)

$(B)/gen/gwbasic.c: $(B)/gwbasic/GWBASIC.EXE $(B)/gwbasic/GWBASIC.MAP $(GWB_LISTINGS) $(wildcard translator/*.py)
	$(PY) -m translator $(B)/gwbasic/GWBASIC.EXE $(GWB_LISTINGS) --map $(B)/gwbasic/GWBASIC.MAP --name GWBASIC.EXE --symbol image_gwbasic -o $@

# DRAW enters four DB-encoded NEGDE macros through its source DW table.
# Keep the established gwbasic.c byte-identical; add only the proved starts.
$(B)/gen/gwbasic_graphics.c: $(B)/gwbasic/GWBASIC.EXE $(B)/gwbasic/GWBASIC.MAP $(GWB_LISTINGS) $(wildcard translator/*.py)
	$(PY) -m translator $(B)/gwbasic/GWBASIC.EXE $(GWB_LISTINGS) --map $(B)/gwbasic/GWBASIC.MAP --supplement gwbasic-graphics --name GWBASIC.EXE --symbol run_gwbasic_graphics -o $@

.PHONY: gwbasic
