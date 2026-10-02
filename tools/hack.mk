# Original Hack 1.0.3, 8086 real-mode DOS, OpenWatcom large memory model.
HACK_INPUTS := $(wildcard third_party/hack/*.[ch] runtime/hack_dos/*.[ch] \
                runtime/hack_dos/sys/*.h) tools/build_hack.py tools/hack_port.py \
                $(addprefix third_party/hack/,data help hh rumors COPYRIGHT COPYRIGHT-JF)
HACK_DATA := $(addprefix $(B)/hack/,data help hh rumors record perm COPYRIGHT COPYRIGHT-JF OWLIC.TXT)

.PHONY: hack
hack: $(B)/hack/HACK.EXE $(HACK_DATA)

$(B)/hack/HACK.EXE $(B)/hack/HACK.MAP $(HACK_DATA) &: $(HACK_INPUTS)
	$(PY) tools/build_hack.py --out $(B)/hack

$(B)/gen/hack.c: $(B)/hack/HACK.EXE $(B)/hack/HACK.MAP $(wildcard translator/*.py)
	@mkdir -p $(B)/gen
	$(PY) -m translator $(B)/hack/HACK.EXE --format watcom \
		--map $(B)/hack/HACK.MAP --name HACK.EXE --symbol image_hack -o $@
