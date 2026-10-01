# Included by the main Makefile after B/PY have been defined.
VZ_MODULES := $(shell $(PY) tools/build_vz.py --modules)
VZ_LISTINGS := $(addprefix $(B)/vz/,$(addsuffix .lst,$(VZ_MODULES)))
VZ_INPUT := $(wildcard third_party/vzeditor/SRC/*) third_party/vzeditor/UPSTREAM third_party/vzeditor/VZ-IBM/US/VZUS.COM tools/build_vz.py tools/vz.mk tools/jwasm/jwasm tools/jwlink/jwlink

$(B)/vz/VZ.COM $(B)/vz/VZ.MAP $(VZ_LISTINGS) &: $(VZ_INPUT)
	$(PY) tools/build_vz.py $(B)/vz

vz: $(B)/vz/VZ.COM $(B)/vz/VZ.MAP $(VZ_LISTINGS)
	cmp $(B)/vz/VZ.COM third_party/vzeditor/VZ-IBM/US/VZUS.COM

$(B)/gen/vz.c: $(B)/vz/VZ.COM $(B)/vz/VZ.MAP $(VZ_LISTINGS) $(wildcard translator/*.py)
	$(PY) -m translator $(B)/vz/VZ.COM $(VZ_LISTINGS) --map $(B)/vz/VZ.MAP --name VZ.COM --symbol image_vz -o $@

test-vz-build: vz
	$(PY) -m pytest -q tests/test_vz_build.py

.PHONY: vz test-vz-build
