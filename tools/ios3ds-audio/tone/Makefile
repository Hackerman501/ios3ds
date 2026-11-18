# ios3ds-tone: AudioQueue test tone (armv6 iOS 4.2 Mach-O), see tone.c.
#   make  -> build/rootfs-add/usr/libexec/ios3ds-tone
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-tone

$(OUT): tone.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra tone.c -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
