# ios3ds-a80211.dylib: the Apple80211 driver surface for configd's WiFiManager
# (interposed ioctl/IOKit calls, backed by kern.ios3ds_wlan; docs/wifi.md 10).
#   make  -> build/rootfs-add/usr/lib/ios3ds-a80211.dylib
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/lib/ios3ds-a80211.dylib

$(OUT): a80211.c $(ROOT)/boot/common/wifi_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/boot/common -dynamiclib \
	    -install_name /usr/lib/ios3ds-a80211.dylib a80211.c \
	    -framework IOKit -framework CoreFoundation -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
