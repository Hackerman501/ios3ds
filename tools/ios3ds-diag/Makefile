# ios3ds-diag: runs a test/diagnostic launchd job only with boot.cfg diag=1 (docs/boot-perf.md)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-diag
LOG  ?= $(ROOT)/build/rootfs-add/usr/lib/ios3ds-applog.dylib

all: $(OUT) $(LOG)

$(OUT): diag.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra diag.c -o $@

# app NSLog -> /dev/console with diag=1 (applog.c, re-log 62)
$(LOG): applog.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -dynamiclib -install_name /usr/lib/ios3ds-applog.dylib applog.c -o $@

clean:
	rm -f $(OUT) $(LOG)
.PHONY: all clean
