# vcd.mediacapture: Celestial's "virtual camera" plug-in slot, backed by the
# 3DS cameras (sysctl ios3ds_cam; docs/camera.md).
#   make  -> build/rootfs-add/System/Library/MediaCapture/vcd.mediacapture
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/System/Library/MediaCapture/vcd.mediacapture

$(OUT): vcd.c $(ROOT)/boot/common/cam_proto.h $(ROOT)/boot/common/cam_pattern.h
	mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/boot/common -dynamiclib \
	    -install_name /System/Library/MediaCapture/vcd.mediacapture vcd.c \
	    -framework CoreFoundation -o $@

clean:
	rm -f $(OUT)
.PHONY: clean
