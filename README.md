<div align="center">

# ios3ds

**Apple's real iOS 4.2.1 running natively on a Nintendo 3DS.**

Not an emulator: the iPod touch 2G's own kernel, SpringBoard and apps run directly on the 3DS's ARM11 CPU.

[![Build](https://github.com/devilAPI/ios3ds/actions/workflows/build.yml/badge.svg)](https://github.com/devilAPI/ios3ds/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/devilAPI/ios3ds?include_prereleases&label=release)](https://github.com/devilAPI/ios3ds/releases)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

[**Install**](#install) · [Controls](#controls) · [What works](#what-works) · [Troubleshooting](#troubleshooting) · [How it works](#how-it-works) · [Build from source](#build-from-source)

</div>

---

## What works

iOS boots to the home screen in about a minute. Apps open, Safari loads web pages over Wi-Fi, and Cydia comes pre-installed.

| | Feature | Notes |
|:-:|---|---|
| ✅ | Boot to the home screen, built-in apps | about a minute from power-on |
| ✅ | Touchscreen, HOME, POWER, START/SELECT, L/R | see [Controls](#controls). Fixed for the next build: touch could stop reacting after a popup or after unlocking |
| ✅ | Graphics on the 3DS GPU (PICA200) | the iOS interface is drawn by the GPU |
| ✅ | Sound, volume slider | speaker |
| ✅ | Battery level, charging, date and time, brightness | through the 3DS system controller |
| ✅ | Wi-Fi: scan, WPA2, also routers in "WPA2 + WPA3" mode | needs the console's own Wi-Fi firmware, copied automatically. Fixed for the next build: the connection dropped after a few minutes (the router's group key renewal went unanswered) |
| ✅ | Safari | loads pages; the first load sometimes fails with "cannot allocate memory", a retry works |
| ✅ | Cydia | pre-installed |
| ✅ | Screen rotation (accelerometer) | portrait came up upside down: fixed for the next build |
| ✅ | D-pad page swipes | turned 90° to match the upright iOS screen in the next build |
| ❌ | Cameras | the sensors are found and start, but no picture arrives yet: being worked on |
| 🧪 | Circle pad scrolling | |
| 🧪 | Headphone jack, microphone | iOS switches to headphones when plugged in |
| 🧪 | Gyroscope (CoreMotion) | |
| 🧪 | Current HTTPS root certificates | TLS 1.0 only, see [Limitations](#limitations) |
| 🧪 | New 3DS: 804 MHz CPU, C-stick, ZL/ZR | L2 cache optional, see [docs/n3ds.md](docs/n3ds.md) |
| ❌ | 3D screen, IR, NFC, video recording | |

✅ works on a real 3DS · 🧪 works in the emulator, not yet confirmed on a console · ❌ not working / not supported

### Compatibility

| Console | Status |
|---|---|
| **Old 3DS** | ✅ supported, the main test console |
| **Old 3DS XL** | ✅ supported (same hardware as the Old 3DS) |
| **Old 2DS** | ✅ supported (same hardware as the Old 3DS) |
| **New 2DS XL** | ✅ boots to the home screen with sound, power button and battery ([#1](https://github.com/devilAPI/ios3ds/issues/1)) |
| New 3DS, New 3DS XL | untested; expected to behave like the New 2DS XL. On all New models the extra CPU cores, RAM, C-stick and ZL/ZR are not used |

All models need Luma3DS (boot9strap). Reports for any model are welcome in the [issues](https://github.com/devilAPI/ios3ds/issues).

---

## Install

You need about 15 minutes, a PC and your own copy of the iPod touch firmware. Nothing is installed to the 3DS itself; everything lives on the SD card.

### What you need

- A 3DS with **Luma3DS** (boot9strap) and the **Homebrew Launcher**, set up the usual way ([3ds.hacks.guide](https://3ds.hacks.guide)).
- An SD card with about **1.2 GB** free.
- A PC with **Python 3.8+** and internet access, needed once to fetch the firmware keys and Cydia:

  | System | Setup |
  |---|---|
  | Linux | `sudo apt install python3 python3-cryptography` |
  | macOS | Python 3, `pip3 install cryptography`, `xcode-select --install` |
  | Windows | use WSL (Ubuntu) and follow the Linux steps; the SD card is `/mnt/<letter>` |

### 1. Download

1. **ios3ds:** `ios3ds-<version>.zip` from the [Releases page](https://github.com/devilAPI/ios3ds/releases). Unzip it anywhere.
2. **The iOS firmware** (ios3ds can't ship it): `iPod2,1_4.2.1_8C148_Restore.ipsw`, the iPod touch 2nd generation on iOS 4.2.1, from Apple's servers via [ipsw.me](https://ipsw.me/download/iPod2,1/8C148).

### 2. Run the installer

Put the SD card in your PC, open a terminal in the unzipped folder and run:

```sh
python3 install.py ~/Downloads/iPod2,1_4.2.1_8C148_Restore.ipsw /media/$USER/YOUR_SD_CARD
```

Use your own paths. The card is `/Volumes/NAME` on macOS and `/mnt/e` (or another letter) in WSL.

It takes a few minutes and needs about 4 GB of free space on the PC while it works. It:
1. decrypts the kernel and the iOS disk from your IPSW, and checks that it is the right file;
2. patches the kernel and builds the iOS disk, including Cydia (6.5 MB from apt.saurik.com; add `--no-cydia` to skip it);
3. writes three things to the card and touches nothing else:

| On the SD card | What it is |
|---|---|
| `3ds/ios3ds.3dsx` | the app you start from the Homebrew Launcher |
| `luma/payloads/ios3ds.firm` | the same, for Luma's chainloader menu |
| `ios3ds/` | iOS itself (`rootfs.hfs`, 1 GB), the patched kernel, and `boot.cfg` (settings) |

Without an SD card path, the files go to `ios3ds-sd/`. Copy what is **inside** that folder to the root of the card.

### 3. Start it

Open the **Homebrew Launcher**, start **ios3ds** and press **A**. The 3DS reboots into iOS.

Alternatively, hold **START** while powering on and pick `ios3ds` in Luma's chainloader menu.

**First start:**
- The launcher copies your console's Wi-Fi firmware to the card, reading it only. It shows `Wi-Fi firmware: reading NWM .code ...`, writes one file, `ios3ds/nwm.code`, and skips this step afterwards.
- The bottom screen shows the version, `SD ok`, `ARM11 ok` and a short countdown. Hold **B** to stop there.
- The top screen scrolls a boot log. After about a minute the iOS home screen appears.
- iOS is upright, so turn the 3DS 90° clockwise. The bottom screen shows a copy of iOS that you tap with the stylus.

> **Wi-Fi:** in iOS, open Settings → Wi-Fi and pick your network.
>
> If the launcher showed a red `Wi-Fi fw: ...` line, copy the firmware once with GodMode9 instead:
> 1. Hold START while powering on and choose GodMode9.
> 2. Press HOME → `Scripts...` → `ios3ds-wifi-fw`, then confirm with A.
>
> Details: [docs/wifi.md](docs/wifi.md) §3.

---

## Controls

| 3DS | iOS |
|---|---|
| Touchscreen | tap, swipe, type |
| **HOME** or **START** | Home button |
| **POWER** (press) or **SELECT** | lock / sleep |
| **L / R** | volume down / up |
| **Circle pad** | scroll (in the direction of the upright iOS screen) |
| **D-pad** | with the 3DS turned so iOS is upright: ← → previous / next page, ↑ ↓ scroll by about a page |
| **X** | screen layout: iOS on top + touch copy below → iOS on the bottom only → iOS on top only |
| **C-stick** (New 3DS) | like the D-pad: flick ← → = previous / next page, ↑ ↓ = scroll by about a page |
| **ZL / ZR** (New 3DS) | scroll to the top / screenshot |
| **POWER** (hold ~10 s) | leave iOS: the 3DS switches off; the next power-on is your normal Luma + HOME Menu |
| SELECT + START (hold) | debugging only: crash iOS on purpose and write a report to the log |

---

## Is it safe?

**ios3ds never writes to the 3DS's internal memory (NAND) or firmware.** It has no code that could. It only reads and writes files on the SD card.

A homebrew app can't start another system directly, so the launcher swaps files on the SD card and reboots:
1. It creates `ios3ds/swap.flg`, a "swap in progress" note.
2. It renames your `boot.firm` (Luma) to `ios3ds/luma.firm`.
3. It renames `ios3ds/ios3ds.firm` to `boot.firm`.
4. It reboots.

The first thing ios3ds does is undo this, so Luma is back as `boot.firm` before iOS even starts. These are renames, never copies, so a power cut can't leave a half-written `boot.firm`. If it ever stops halfway:
- the 3DS falls back to the backup `boot.firm` in its internal storage (set up by the 3ds.hacks.guide);
- the launcher offers **Y: restore Luma** when it finds a leftover `ios3ds/luma.firm`;
- or rename `ios3ds/luma.firm` back to `boot.firm` on a PC.

As with any homebrew, **back up your SD card** first. More: [docs/real-hardware.md](docs/real-hardware.md).

---

## Troubleshooting

- **Logs:** every boot writes **`sd:/ios3ds/log.txt`**, and the previous one is kept as `log_prev.txt`. Please [open an issue](https://github.com/devilAPI/ios3ds/issues/new/choose) and attach `log.txt`.
- **Settings:** they live in **`sd:/ios3ds/boot.cfg`**, a text file. Useful switches:

  | Switch | Effect |
  |---|---|
  | `gpu=0` | don't use the 3DS GPU at all: try this first if the screen stays black or garbled |
  | `ca_gpu=0` | draw the iOS interface in software (slower) |
  | `screen=0/1/2` | start in screen layout top / both / bottom |
  | `camera=0`, `gyro=0`, `hp_route=0` | turn off the cameras, the gyroscope, or headphone switching |
  | `diag=1` | extra diagnostics in the log |

- **Re-running the installer** replaces `boot.cfg`; the old one is kept as `boot.cfg.old`. The installer refuses a card with a leftover `ios3ds/luma.firm`: start ios3ds once, or restore Luma first.
- **Uninstall:** delete `ios3ds/`, `3ds/ios3ds.3dsx` and `luma/payloads/ios3ds.firm` from the card.

### Limitations

- **iOS 4.2.1 is the newest possible:** later versions need an ARMv7 CPU, and the 3DS's ARM11 is ARMv6.
- **HTTPS:** iOS 4.2.1 speaks TLS 1.0 at most. Sites that require TLS 1.2 or newer (GitHub, Wikipedia, apple.com, …) can't be opened. Current root certificates are added, so sites that still allow TLS 1.0 work ([docs/certificates.md](docs/certificates.md)).
- **OpenGL ES apps and games:** 3D runs through our own OpenGL ES 1.1 engine. There is no ES 2.0 and no PVRTC textures.
- **Not supported:** activation, iTunes sync and the data-protection keybag are stubbed out.

---

## How it works

```
3DS boot (Luma3DS chainloader)
 ├─ ARM9   our "monitor": both screens' consoles, SD card, the iOS root disk (served from rootfs.hfs),
 │          Wi-Fi (AR6014 driver + WPA2), system controller, cameras, gyro, screen copy
 └─ ARM11  Apple's XNU kernel, stock, + binary patches (kernel/patches.json) + our "shim"
            ├─ timers, interrupts, console → mapped onto the 3DS's MPCore
            ├─ display → Apple's own LCD driver on a fake register page; the PICA200 GPU copies to the LCDs
            ├─ audio, touch, buttons, battery, Wi-Fi … → mailboxes to the ARM9
            └─ launchd → configd, mediaserverd, … → SpringBoard → your apps
```

Apple's kernel believes it is running on an iPod touch. Where the 3DS hardware differs, small patches and the shim translate. The ARM9 handles everything slow or Nintendo-specific. Full picture: [docs/architecture.md](docs/architecture.md).

The project has its own QEMU machine (`-M nintendo3ds`, [docs/emulator.md](docs/emulator.md)) that models the 3DS hardware, from the GPU to Wi-Fi, cameras and gyro. Every change is tested there before it reaches a console. Citra/Azahar only emulate 3DS *apps*; they can run the launcher but not ios3ds itself.

---

## Build from source

Linux. Requirements: Python 3 with `cryptography`, an `arm-none-eabi` toolchain (Arm GNU Toolchain or devkitARM), `mtools`, `dosfstools`, `sfdisk`, git.

```sh
make toolchain                                       # check the tools
make fwprep IPSW=~/iPod2,1_4.2.1_8C148_Restore.ipsw   # decrypt kernel, device tree, rootfs -> build/staging
scripts/build-hfsplus.sh                             # HFS+ image editor (xpwn)
scripts/build-ios-toolchain.sh                       # armv6 iOS toolchain for our iOS programs (once)
make userland                                        # our iOS programs -> build/rootfs-add/
make sd                                              # SD card tree -> build/sd/  (copy its contents to the card)
make test                                            # offline unit tests
```

Optional:
- `make qemu` builds the emulator, about 10 minutes.
- `make emu` boots `build/sd.img` in it.
- `make launcher` builds `ios3ds.3dsx` and needs devkitARM + libctru.
- `make release` builds the release zip ([docs/release.md](docs/release.md)).

**No Apple files in this repository.** You supply the IPSW, and everything Apple-derived is generated into `build/` (gitignored). The patch recipes hold only addresses and the few original bytes needed to check that the right firmware is being patched.

### Documentation

| | |
|---|---|
| [architecture.md](docs/architecture.md) | boot chain, memory map, every hardware bridge |
| [real-hardware.md](docs/real-hardware.md) | testing safely on a console |
| [emulator.md](docs/emulator.md) | the QEMU `nintendo3ds` machine, Azahar |
| [gpu.md](docs/gpu.md) · [pica200.md](docs/pica200.md) | OpenGL ES engine, CoreAnimation and the screen copy on the PICA200 |
| [audio.md](docs/audio.md) | speaker, headphones, microphone |
| [mcu.md](docs/mcu.md) | battery, clock, buttons, sliders, accelerometer, gyroscope |
| [wifi.md](docs/wifi.md) · [certificates.md](docs/certificates.md) | Wi-Fi driver, WPA2, iOS integration; HTTPS roots |
| [camera.md](docs/camera.md) | cameras in Camera.app |
| [input-design.md](docs/input-design.md) | touch, buttons, circle pad |
| [cydia.md](docs/cydia.md) | the pre-installed Cydia |
| [re-log.md](docs/re-log.md) | the reverse-engineering log, step by step |
| [hwlogs/](docs/hwlogs/README.md) | logs from real hardware runs |

---

## Credits

- [samhenrigold/qemu-ios](https://github.com/samhenrigold/qemu-ios) (Light Touch): research on booting 8C148, invaluable.
- [devos50/qemu-ios](https://github.com/devos50/qemu-ios): iPod touch emulation and S5L8720 notes.
- [darwin-on-arm](https://github.com/darwin-on-arm/xnu) (winocm): boot_args and pexpert reference.
- [libn3ds](https://github.com/profi200/libn3ds), [Luma3DS](https://github.com/LumaTeam/Luma3DS), [linux-3ds](https://github.com/linux-3ds), [dsiwifi](https://github.com/shinyquagsire23/dsiwifi), GBATEK and 3dbrew: 3DS hardware references.
- Normmatt's SD driver, FatFs by ChaN, [xpwn](https://github.com/planetbeing/xpwn), and The Apple Wiki.
- Everyone testing on real consoles in the [issues](https://github.com/devilAPI/ios3ds/issues).

## License

GPL-3.0-or-later ([LICENSE](LICENSE)), with these exceptions:
- `boot/arm9/sdmmc.c` and `emu/qemu/` are GPL-2.0-or-later.
- `boot/arm9/fatfs/` is under FatFs's 1-clause BSD licence.
- `boot/common/font8x8_basic.h` is public domain.

iOS, iPod and XNU are trademarks of Apple; Nintendo 3DS is a trademark of Nintendo. This is an independent research project, not affiliated with either.
