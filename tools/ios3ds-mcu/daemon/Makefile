# ios3ds-mcud: 3DS MCU mailbox -> iOS (volume, brightness, accelerometer; docs/mcu.md)
#   make            -> build/rootfs-add/usr/libexec/ios3ds-mcud
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-mcud

$(OUT): mcud.c $(ROOT)/boot/common/mcu_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -I$(ROOT)/boot/common mcud.c -framework IOKit -framework CoreFoundation -lobjc -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
