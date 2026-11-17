# ios3ds-pica: PICA200 support library for the ios3ds GL engine. See docs/pica200.md.
#   make test     unit tests on the build host (encoders, texture tiling,
#                 fixed-function mapping, driver core + software GPU model)
#   make shader   regenerate src/pica_ff_shader.h from shader/ff.vsh
#   make mirror   build/pica-host/gpumirror.bin: the GPU display mirror's
#                 command lists (scripts/mkboot.py puts them into gpu.bin)
# The library itself is compiled into tools/ios3ds-gl (engine), kernel/shim
# (driver core) and QEMU (sim/picasim.c); there is nothing to install here.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT  := $(ROOT)/build/pica-host
SRC  := src/pica_cmd.c src/pica_state.c src/pica_tex.c src/pica_ff.c src/pica_drv.c src/pica_dev_host.c sim/picasim.c
ENC  := src/pica_cmd.c src/pica_state.c src/pica_ff.c
INC  := -Iinclude -I$(ROOT)/boot/common

test: $(OUT)/test_pica $(OUT)/test_mirror $(OUT)/gpumirror.bin
	$(OUT)/test_pica
	$(OUT)/test_mirror $(OUT)/gpumirror.bin

mirror: $(OUT)/gpumirror.bin

$(OUT)/genmirror: mirror/genmirror.c $(ENC) $(wildcard include/*.h src/*.h) $(ROOT)/boot/common/gpu_mirror.h
	mkdir -p $(OUT)
	cc -O2 -g -Wall -Wextra $(INC) $(ENC) mirror/genmirror.c -lm -o $@

$(OUT)/gpumirror.bin: $(OUT)/genmirror
	$(OUT)/genmirror $@

# the mirror engine (src/pica_mirror.c) against the model, compared with the
# ARM9's own CPU copy (boot/arm9/display.c, compiled for the host)
$(OUT)/test_mirror: tests/test_mirror.c src/pica_mirror.c src/pica_drv.c src/pica_diag.c sim/picasim.c $(ROOT)/boot/arm9/display.c \
		$(wildcard include/*.h sim/*.h) $(ROOT)/boot/common/gpu_mirror.h
	mkdir -p $(OUT)
	cc -O2 -g -Wall -Wextra -Wno-int-to-pointer-cast $(INC) -I$(ROOT)/boot/arm9 tests/test_mirror.c \
		src/pica_mirror.c src/pica_drv.c src/pica_diag.c sim/picasim.c $(ROOT)/boot/arm9/display.c -lm -o $@

$(OUT)/test_pica: $(SRC) tests/test_pica.c $(wildcard include/*.h sim/*.h src/*.h)
	mkdir -p $(OUT)
	cc -O2 -g -Wall -Wextra -Iinclude $(SRC) tests/test_pica.c -lm -o $@

shader:
	python3 picasm.py shader/ff.vsh -o src/pica_ff_shader.h --name pica_ff

clean:
	rm -rf $(OUT)
.PHONY: test shader mirror clean
