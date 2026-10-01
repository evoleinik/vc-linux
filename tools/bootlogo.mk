# Included by the main Makefile after B/PY have been defined.
NASM := tools/nasm/nasm
BOOTLOGO_INPUT := third_party/bootlogo/bootlogo.asm tools/bootlogo.mk $(NASM)

# -Le identifies active, expanded source, -Lf/-LF expose hidden rows, and
# -Lt gives every byte of TIMES/ALIGN instead of an abbreviated repeat marker.
$(B)/bootlogo/LOGO.COM $(B)/bootlogo/LOGO.lst &: $(BOOTLOGO_INPUT)
	@mkdir -p $(B)/bootlogo
	$(NASM) -Dcom_file=1 -f bin -LefFt -l $(B)/bootlogo/LOGO.lst -o $(B)/bootlogo/LOGO.COM third_party/bootlogo/bootlogo.asm

bootlogo: $(B)/bootlogo/LOGO.COM $(B)/bootlogo/LOGO.lst

$(B)/gen/bootlogo.c: $(B)/bootlogo/LOGO.COM $(B)/bootlogo/LOGO.lst $(wildcard translator/*.py)
	$(PY) -m translator $(B)/bootlogo/LOGO.COM $(B)/bootlogo/LOGO.lst --format nasm --name LOGO.COM --symbol image_bootlogo -o $@

.PHONY: bootlogo
