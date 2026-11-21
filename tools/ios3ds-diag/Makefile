# ios3ds-diag: runs a test/diagnostic launchd job only with boot.cfg diag=1 (docs/boot-perf.md)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-diag
LOG  ?= $(ROOT)/build/rootfs-add/usr/lib/ios3ds-applog.dylib

all: $(OUT) $(LOG)

# ent.plist: SpringBoard wants com.apple.springboard.opensensitiveurl for -urlpoll
$(OUT): diag.c ent.plist
	mkdir -p $(dir $@)
	IOS_CC_ENTITLEMENTS=$(CURDIR)/ent.plist $(CC) -Os -Wall -Wextra diag.c -o $@

# app NSLog -> /dev/console with diag=1 (applog.c, re-log 62)
$(LOG): applog.c ../ios3ds-gps/gps.c ../ios3ds-gps/gps.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -I../ios3ds-gps -dynamiclib -install_name /usr/lib/ios3ds-applog.dylib applog.c ../ios3ds-gps/gps.c -o $@

clean:
	rm -f $(OUT) $(LOG)
.PHONY: all clean
