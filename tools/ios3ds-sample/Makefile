# ios3ds-sample: all-process thread sampler (armv6 iOS 4.2 Mach-O).
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  := $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-sample

$(OUT): sample.c ent.plist
	mkdir -p $(dir $@)
	IOS_CC_ENTITLEMENTS=$(CURDIR)/ent.plist $(CC) -Os -Wall -Wextra sample.c -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
