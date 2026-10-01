# Original Rogue 5.4.4 + PDCurses, 8086 DOS large-model build.
# Set WATCOM to an OpenWatcom installation; no toolchain files are vendored.
ROGUE_INPUTS := $(wildcard third_party/rogue/*.[ch] third_party/pdcurses/*.h \
                 third_party/pdcurses/pdcurses/*.c third_party/pdcurses/dos/*.[ch] \
                 runtime/rogue_dos/*.[ch]) tools/build_rogue.py tools/rogue_port.py

.PHONY: rogue
rogue: $(B)/rogue/ROGUE.EXE

$(B)/rogue/ROGUE.EXE $(B)/rogue/ROGUE.MAP $(B)/rogue/OWLIC.TXT &: $(ROGUE_INPUTS)
	$(PY) tools/build_rogue.py --out $(B)/rogue

$(B)/gen/rogue.c: $(B)/rogue/ROGUE.EXE $(B)/rogue/ROGUE.MAP $(wildcard translator/*.py)
	@mkdir -p $(B)/gen
	$(PY) -m translator $(B)/rogue/ROGUE.EXE --format watcom \
		--map $(B)/rogue/ROGUE.MAP --name ROGUE.EXE --symbol image_rogue -o $@
