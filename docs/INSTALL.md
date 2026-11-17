# Installing ios3ds on your 3DS

ios3ds runs Apple's iOS 4.2.1 (the iPod touch 2G firmware) on a 3DS. It is an experiment: it boots to the iOS home screen and apps open, but it is slow-ish and some hardware does not work yet. It is tested on an **Old 3DS**; other models are untested.

## What you need

- A 3DS with **Luma3DS** (boot9strap) and the **Homebrew Launcher**, set up the usual way ([3ds.hacks.guide](https://3ds.hacks.guide)).
- An SD card with about **1.2 GB** free.
- A PC with **Python 3.8+** and an internet connection (once, to fetch the firmware keys).
  - **Linux:** `sudo apt install python3 python3-cryptography` (or `pip install cryptography`).
  - **macOS:** Python 3 from python.org or Homebrew, `pip3 install cryptography`, and `xcode-select --install` (the installer compiles one small tool).
  - **Windows:** use WSL (Ubuntu) and follow the Linux steps. Your SD card is under `/mnt/<drive letter>`, e.g. `/mnt/e`.

## 1. Download two files

1. **The ios3ds release:** `ios3ds-<version>.zip` from the [Releases page](https://github.com/devilAPI/ios3ds/releases). Unzip it anywhere.
2. **The iOS firmware**, which ios3ds can't ship for you: `iPod2,1_4.2.1_8C148_Restore.ipsw` (iPod touch 2nd generation, iOS 4.2.1). Download it from Apple's servers through [ipsw.me](https://ipsw.me/download/iPod2,1/8C148).

## 2. Run the installer

Put the SD card in your PC. Open a terminal in the unzipped `ios3ds-<version>` folder and run:

```sh
python3 install.py ~/Downloads/iPod2,1_4.2.1_8C148_Restore.ipsw /media/$USER/YOUR_SD_CARD
```

Use your own paths to the IPSW and to the SD card. On macOS the card is `/Volumes/NAME`; in WSL it is `/mnt/e` (or another letter).

It takes a few minutes and needs about 4 GB of free space on the PC while it works. It:
1. decrypts the kernel and the iOS disk from your IPSW (and checks that it is the right file);
2. patches the kernel and builds the iOS disk with the ios3ds programs;
3. copies the result to the card.

Nothing else on the card is touched. If you leave out the SD card path, the files go to a folder `ios3ds-sd/` instead; copy what is **inside** it to the root of the SD card.

These files are now on the card:

| File | What it is |
|---|---|
| `3ds/ios3ds.3dsx` | the app for the Homebrew Launcher |
| `luma/payloads/ios3ds.firm` | the same thing for Luma's chainloader menu |
| `ios3ds/` | iOS itself (`rootfs.hfs`, 1 GB), the patched kernel, and `boot.cfg` (settings) |

Safely eject the card and put it back in the 3DS.

## 3. Start it

Open the **Homebrew Launcher**, start **ios3ds**, and press **A**. The 3DS reboots into ios3ds.

Or: hold **START** while powering on, and pick `ios3ds` in Luma's chainloader menu.

**First boot:**
- The bottom screen shows the version, "SD ok", "ARM11 ok" and a short countdown (hold **B** during it to stay on this screen).
- The top screen scrolls a boot log.
- After **about a minute** the iOS home screen appears. iOS is upright, so turn the 3DS 90° clockwise to read it.
- The bottom screen shows a copy of iOS that you **tap** with the stylus.
- Everything is slower than on a real iPod touch.

## Controls

| Button | Does |
|---|---|
| Touchscreen | tap and swipe in iOS |
| **START** | Home button |
| **SELECT** | lock / sleep button |
| **L / R** | volume down / up |
| **X** | screen mode: iOS on top + touch copy below (default) → iOS on the bottom screen only → iOS on top only (log below) |
| **SELECT + START** (held) | debugging: makes iOS crash on purpose and writes a report to the log. **Don't press it** unless you are collecting a bug report. |
| **POWER** (hold ~10 s) | leave iOS: the 3DS switches off. Power on again for your normal Luma + HOME Menu. |

## What works, what doesn't

- **Works:** home screen, touch, Home/lock/volume buttons, built-in apps such as Settings, Notes and Mail.
- **No Wi-Fi** by default (experimental, needs a config file, untested on hardware).
- **No sound yet** (work in progress).
- No real clock: the date and time are wrong.
- 3D (OpenGL) apps are drawn in software and are slow.

## Is this safe for my 3DS?

ios3ds **never writes to the 3DS's internal memory (NAND) or its firmware**; it has no code to do so. It only reads and writes files on the SD card. If it hangs, hold POWER for ~10 s; the next boot is your normal setup.

**What the Homebrew Launcher app does:** a homebrew app can't start another system directly, so `ios3ds.3dsx` renames files on the SD card and reboots:
1. creates `ios3ds/swap.flg` (a "swap in progress" note);
2. renames your `boot.firm` (Luma) to `ios3ds/luma.firm`;
3. renames `ios3ds/ios3ds.firm` to `boot.firm`;
4. reboots, so the 3DS starts ios3ds.

The **first thing ios3ds does** when it starts is the reverse: Luma goes back to `boot.firm`, so your next power-on is normal again. These are renames only, never copies, so a power cut can't leave a half-written `boot.firm`. If it ever stops between two steps:
- the 3DS boots its backup `boot.firm` from internal storage (the 3ds.hacks.guide installs one);
- the ios3ds app offers **Y: restore Luma** when it finds a leftover `ios3ds/luma.firm`;
- or, on a PC, rename `ios3ds/luma.firm` on the card back to `boot.firm`.

As with any homebrew, **back up your SD card** first. The worst realistic outcome is a damaged SD card file system. More detail: [real-hardware.md](real-hardware.md).

## If something goes wrong

- Every boot writes a log to **`sd:/ios3ds/log.txt`**. The previous boot's log is kept as `log_prev.txt`. Please attach `log.txt` to bug reports.
- Settings live in **`sd:/ios3ds/boot.cfg`**, a text file. Useful switches:
  - `gpu=0`: don't use the 3DS graphics chip at all (try this first if the screen stays black or garbled);
  - `gpu_mirror=0`: copy the screen to the displays the old way, without the graphics chip;
  - `screen=0`, `1` or `2`: start in screen mode top / both / bottom.
- Running the installer again replaces `boot.cfg`; your old one is kept as `boot.cfg.old`.
- The installer refuses a card with a leftover `ios3ds/luma.firm` (see above): start ios3ds once, or restore Luma first.

To remove ios3ds, delete `ios3ds/`, `3ds/ios3ds.3dsx` and `luma/payloads/ios3ds.firm` from the card.
