# ios3ds-gyrotest: CoreMotion gyro check for diag=1 boots (docs/mcu.md 11)
#   make  -> build/rootfs-add/usr/libexec/ios3ds-gyrotest
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-gyrotest

$(OUT): gyrotest.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra gyrotest.c -lobjc -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
