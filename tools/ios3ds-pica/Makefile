# ios3ds-pica: PICA200 support library for the ios3ds GL engine. See docs/pica200.md.
#   make test     unit tests on the build host (encoders, texture tiling,
#                 fixed-function mapping, driver core + software GPU model)
#   make shader   regenerate src/pica_ff_shader.h from shader/ff.vsh
# The library itself is compiled into tools/ios3ds-gl (engine), kernel/shim
# (driver core) and QEMU (sim/picasim.c); there is nothing to install here.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT  := $(ROOT)/build/pica-host
SRC  := src/pica_cmd.c src/pica_state.c src/pica_tex.c src/pica_ff.c src/pica_drv.c src/pica_dev_host.c sim/picasim.c

test: $(OUT)/test_pica
	$(OUT)/test_pica

$(OUT)/test_pica: $(SRC) tests/test_pica.c $(wildcard include/*.h sim/*.h src/*.h)
	mkdir -p $(OUT)
	cc -O2 -g -Wall -Wextra -Iinclude $(SRC) tests/test_pica.c -lm -o $@

shader:
	python3 picasm.py shader/ff.vsh -o src/pica_ff_shader.h --name pica_ff

clean:
	rm -rf $(OUT)
.PHONY: test shader clean
