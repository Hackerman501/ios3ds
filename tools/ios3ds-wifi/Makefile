# ios3ds-wifi: host unit tests + the userland daemon.
#   make test     host tests (C: ring/frames/config/WPA2, Python: nwmfw.py)
#   make daemon   build/rootfs-add/usr/libexec/ios3ds-wifid (needs scripts/ios-cc)
# SPDX-License-Identifier: GPL-3.0-or-later
ROOT   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
HOSTCC ?= cc
OUT    := $(ROOT)/build/wifi-tests
A9W    := $(ROOT)/boot/arm9/wifi
SRC    := tests/test_host.c $(addprefix $(A9W)/,wifi_ring.c wifi_frame.c wifi_cfg.c wpa2.c)

.PHONY: test daemon clean
test: $(OUT)/test_host $(OUT)/nwm_host
	$(OUT)/test_host
	python3 -m unittest discover -s tests -p 'test_*.py' -v

$(OUT)/test_host: $(SRC) $(A9W)/wifi.h $(A9W)/wpa2.h shim/net_core.h $(ROOT)/boot/common/wifi_proto.h
	@mkdir -p $(OUT)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
	    -fsanitize=address,undefined -DWIFI_HOST_TEST \
	    -I$(ROOT)/boot/common -I$(A9W) -Ishim $(SRC) -o $@

$(OUT)/nwm_host: tests/nwm_host.c $(A9W)/nwmcode.c $(A9W)/wifi.h
	@mkdir -p $(OUT)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
	    -fsanitize=address,undefined -DWIFI_HOST_TEST \
	    -I$(ROOT)/boot/common -I$(A9W) -Ishim tests/nwm_host.c $(A9W)/nwmcode.c -o $@

daemon:
	$(MAKE) -C daemon

clean:
	rm -rf $(OUT)
