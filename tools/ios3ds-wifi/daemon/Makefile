# ios3ds-wifid: kern.ios3ds_net <-> IOUserEthernet bridge (armv6 iOS 4.2 Mach-O).
#   make  -> build/rootfs-add/usr/libexec/ios3ds-wifid + ios3ds-nettest (self-test client)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-wifid

.DEFAULT_GOAL := all
$(OUT): wifid.c ent.plist $(ROOT)/boot/common/wifi_proto.h
	mkdir -p $(dir $@)
	IOS_CC_ENTITLEMENTS=$(CURDIR)/ent.plist $(CC) -Os -Wall -Wextra -I$(ROOT)/boot/common wifid.c \
	    -framework IOKit -framework CoreFoundation -o $@

NETTEST ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-nettest
all: $(OUT) $(NETTEST)

$(NETTEST): nettest.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra nettest.c -o $@

clean:
	rm -f $(OUT) $(NETTEST)
.PHONY: all clean
