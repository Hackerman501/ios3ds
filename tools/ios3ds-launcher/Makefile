# ios3ds launcher (.3dsx) - needs devkitARM + libctru (DEVKITPRO set, e.g.
# `source ../../../rockbox-3ds/scripts/env.sh` or a normal devkitPro install).
#   make  -> ../../build/launcher/ios3ds.3dsx
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set)
endif
OUT   := ../../build/launcher
CTRU  := $(DEVKITPRO)/libctru
CC    := $(DEVKITPRO)/devkitARM/bin/arm-none-eabi-gcc
ARCH  := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -O2 -Wall -Wextra $(ARCH) -D__3DS__ -I$(CTRU)/include
LDFLAGS := -specs=3dsx.specs $(ARCH) -L$(CTRU)/lib

$(OUT)/ios3ds.3dsx: $(OUT)/ios3ds-launcher.elf $(OUT)/ios3ds.smdh
	$(DEVKITPRO)/tools/bin/3dsxtool $< $@ --smdh=$(OUT)/ios3ds.smdh

$(OUT)/ios3ds-launcher.elf: main.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lctru -lm -o $@

$(OUT)/ios3ds.smdh:
	mkdir -p $(OUT)
	$(DEVKITPRO)/tools/bin/smdhtool --create "ios3ds" "Boot iOS 4.2.1 (reboots)" "ios3ds" \
		$(CTRU)/default_icon.png $@

clean:
	rm -rf $(OUT)
.PHONY: clean
