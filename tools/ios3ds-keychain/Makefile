# ios3ds-hwaes.dylib: software IOAESAccelerator for securityd, so the keychain
# works without the iPod's AES engine (docs/wifi.md section 10).
#   make  -> build/rootfs-add/usr/lib/ios3ds-hwaes.dylib
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/lib/ios3ds-hwaes.dylib

$(OUT): hwaes.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -Wno-unused-parameter -dynamiclib -install_name /usr/lib/ios3ds-hwaes.dylib \
	    hwaes.c -framework IOKit -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
