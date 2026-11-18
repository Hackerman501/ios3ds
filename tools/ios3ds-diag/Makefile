# ios3ds-diag: runs a test/diagnostic launchd job only with boot.cfg diag=1 (docs/boot-perf.md)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-diag

$(OUT): diag.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra diag.c -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
