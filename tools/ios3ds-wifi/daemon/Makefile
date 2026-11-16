# ios3ds-wifid: kern.ios3ds_net <-> IOUserEthernet bridge (armv6 iOS 4.2 Mach-O).
#   make  -> build/rootfs-add/usr/libexec/ios3ds-wifid
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-wifid

$(OUT): wifid.c ent.plist $(ROOT)/boot/common/wifi_proto.h
	mkdir -p $(dir $@)
	IOS_CC_ENTITLEMENTS=$(CURDIR)/ent.plist $(CC) -Os -Wall -Wextra -I$(ROOT)/boot/common wifid.c \
	    -framework IOKit -framework CoreFoundation -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
