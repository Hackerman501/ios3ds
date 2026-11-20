# ios3ds top-level Makefile
#
#   make toolchain            check the cross compiler / python deps
#   make fwprep IPSW=path     decrypt kernel, device tree, rootfs into build/staging
#   make firm                 3DS payload  -> build/boot/ios3ds.firm
#   make shim                 kernel shim  -> build/shim/{shim.bin,gpu.bin} (prebuilt for releases)
#   make launcher             Homebrew Launcher app -> build/launcher/ios3ds.3dsx (devkitARM + libctru)
#   make userland             our iOS binaries -> build/rootfs-add/ (scripts/build-ios-toolchain.sh)
#   make hfsplus              xpwn's HFS+ editor -> build/tools/hfsplus
#   make sd                   SD card tree -> build/sd/  (copy its contents to the card root;
#                             downloads Cydia's debs into build/cydia, CYDIA=0 to skip)
#   make sdimg                same, plus a 2 GiB raw SD image for the emulator (build/sd.img)
#   make qemu                 build QEMU with the 'nintendo3ds' dev machine (build/qemu)
#   make emu                  boot build/sd.img in the emulator, print the SD log
#   make kscan                print kernel facts used by docs/research.md
#   make test                 offline unit tests (no Apple files needed)
#   make release VERSION=v1   release zip of our own builds -> build/release/ (docs/release.md)
#
# Nothing Apple-owned is ever written outside build/ (gitignored).

PYTHON  ?= python3
# CYDIA=0: build the rootfs without the downloaded Cydia/APT base system (docs/cydia.md)
ifneq ($(filter 0 no off,$(CYDIA)),)
MKSD_FLAGS += --no-cydia
endif
BUILD   ?= build
STAGING := $(BUILD)/staging

USERLAND := tools/ios3ds-diag tools/ios3ds-sample tools/ios3ds-gl tools/ios3ds-input/daemon tools/ios3ds-wifi/daemon tools/ios3ds-wifi/a80211 tools/ios3ds-keychain tools/ios3ds-audio/tone tools/ios3ds-mcu/daemon tools/ios3ds-mcu/gyrotest tools/ios3ds-camera/vcd

.PHONY: all help toolchain fwprep firm shim launcher userland hfsplus sd sdimg qemu emu kscan test release clean

all: firm

help:
	@sed -n '3,19p' Makefile | sed 's/^# //'

toolchain:
	@scripts/setup-toolchain.sh

fwprep:
	@test -n "$(IPSW)" || { echo "usage: make fwprep IPSW=/path/to/iPod2,1_4.2.1_8C148_Restore.ipsw"; exit 2; }
	$(PYTHON) scripts/fwprep.py "$(IPSW)" -o $(STAGING) $(FWPREP_FLAGS)

firm:
	$(MAKE) -C boot

shim:
	$(PYTHON) scripts/mkboot.py --shim-only $(BUILD)/shim

launcher:
	$(MAKE) -C tools/ios3ds-launcher

userland:
	for d in $(USERLAND); do $(MAKE) -C $$d || exit 1; done

hfsplus:
	scripts/build-hfsplus.sh

sd: firm
	$(PYTHON) scripts/mksd.py $(MKSD_FLAGS)

sdimg: firm
	$(PYTHON) scripts/mksd.py --image $(BUILD)/sd.img $(MKSD_FLAGS)

qemu:
	emu/build-qemu.sh

emu: sdimg
	$(PYTHON) emu/run.py --time $${T:-10} --shots $${SHOTS:-9.5}

kscan: $(STAGING)/kernelcache.macho
	$(PYTHON) scripts/kscan.py $<

$(STAGING)/kernelcache.macho:
	@echo "run 'make fwprep IPSW=...' first"; exit 2

test:
	$(PYTHON) -m unittest discover -s tests -v
	$(MAKE) -C tools/ios3ds-pica test
	$(MAKE) -C tools/ios3ds-gl host-test
	$(MAKE) -C tools/ios3ds-wifi test
	$(CC) -Wall -Wextra -O1 -o $(BUILD)/test_cpad tools/ios3ds-input/tests/test_cpad.c && $(BUILD)/test_cpad
	$(CC) -Wall -Wextra -O1 -o $(BUILD)/test_n3ds tools/ios3ds-input/tests/test_n3ds.c && $(BUILD)/test_n3ds

release:
	$(PYTHON) scripts/mkrelease.py $(if $(VERSION),--version $(VERSION))

clean:
	rm -rf $(BUILD)/boot $(BUILD)/sd $(BUILD)/sd.img $(BUILD)/emu
