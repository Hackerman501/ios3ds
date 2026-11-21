ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  := $(ROOT)/build/rootfs-add/usr/bin/ios3ds-gps
OUT2 := $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-gpstest
TEST := $(ROOT)/build/ios3ds-gps-test

all: $(OUT) $(OUT2)

$(OUT): ios3ds-gps.c gps.c gps.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra ios3ds-gps.c gps.c -framework CoreFoundation -o $@

$(OUT2): gpstest.c gps.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra gpstest.c -framework CoreFoundation -o $@

test: $(TEST)
	$(TEST)

$(TEST): gps.c tests/test_gps.c
	mkdir -p $(dir $@)
	cc -O1 -Wall -Wextra -Werror -I. gps.c tests/test_gps.c -lm -o $@

clean:
	rm -f $(OUT) $(TEST)
.PHONY: all test clean