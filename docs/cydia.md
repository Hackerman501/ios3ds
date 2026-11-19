# Cydia pre-installed

ios3ds images come "pre-jailbroken" the way greenpois0n or redsn0w left an iOS 4.2.1
device: Cydia on the home screen, working `dpkg`/APT, and the base system Cydia
treats as essential. Code signing is already off in our boot-args
(`amfi_allow_any_signature=1 cs_enforcement_disable=1`), so nothing else is needed
for unsigned code to run.

The packages are Saurik's (Telesphoreo). **They are never committed and never in a
release zip.** `make sd` and the installer download them on your machine from the
original repository, check every file against a pinned sha256, and unpack them
into the rootfs image. Without network the image is built without Cydia (one
warning line), never fails.

## What gets installed

`rootfs/cydia.json` pins 32 packages: Cydia and the closure of its `Depends`/
`Pre-Depends`, plus every `Essential: yes` package, each at the newest version in
`http://apt.saurik.com/` `dists/tangelo-3.7` (identical to the `ios` dist; `.deb`
files redirect to `cache.saurik.com`). 6.5 MB download, about 40 MB unpacked
(the volume has ~400 MB free after it).

| Package | Version | | Package | Version |
|---|---|---|---|---|
| cydia | 1.1.30 | | dpkg | 1.18.10-12 |
| cydia-lproj | 1.1.12 | | apt7 / apt7-lib / apt7-key | 0.7.25.3-9 / -16 / -3 |
| base | 1-4 | | bash | 4.0.44-16 |
| coreutils / coreutils-bin | 8.12-13 / 8.12-9 | | debianutils | 3.3.3ubuntu1-1p |
| darwintools | 1-5 | | uikittools | 1.1.12 |
| system-cmds | 433.8-14 | | shell-cmds | 118-7 |
| firmware-sbin | 0-1 | | profile.d | 0-2 |
| sed, grep, findutils, diffutils | 4.1.5-8, 2.5.4-3, 4.2.33-7, 2.8.1-6 | | tar, gzip, bzip2, lzma | 1.29-10, 1.6-7, 1.0.5-8, 4.32.7-5 |
| gnupg, berkeleydb | 1.4.8-4, 4.6.21-5 | | ncurses, readline | 5.7-15, 6.0-8 |
| pam, pam-modules | 32.1-4, 36.1-5 | | org.thebigboss.repo.icons | 1.0 |

Why these versions: Cydia 1.1.30 (2017) is the last release and still has an
**armv6** slice with `LC_VERSION_MIN_IPHONEOS 2.0`; it links only libraries 4.2.1
has (libstdc++, WebKit/WebCore, SpringBoardServices, ...). Every Mach-O in the set
was checked to have an armv6 or generic-ARM slice. The 2011 package versions a
greenpois0n bootstrap installed are no longer all served, and an old Cydia would
immediately offer these upgrades anyway.

Not installed: **Cydia Substrate (MobileSubstrate)**. It injects into every process
through `DYLD_INSERT_LIBRARIES`, which our `configd` already uses for the Wi-Fi bridge
(`rootfs/recipe.json`), and SpringBoard runs our GL/CA environment; it stays out
until it is tested against those. Install it from Cydia at your own risk.

## What the build does (scripts/cydia.py)

Hooked into `scripts/mkrootfs.py` right after the volume is grown and before our own
files are written (so the recipe always wins over a deb):

1. **Fetch**: for each pinned entry, `mirrors[i] + path` -> `build/cydia/` (installer:
   `<work>/cydia/`, deleted afterwards). A cached file whose sha256 matches is reused.
   Any failure (offline, 404, wrong size/hash) -> `[cydia] WARNING ... building
   WITHOUT Cydia`, and the image is built as before.
2. **Unpack** each `data.tar.{lzma,gz,bz2,xz}` with xpwn's `hfsplus`: directories
   (`mkdir`, mode), files (`add` + `chmod`, keeping setuid bits such as
   `cydo` 6755), symlinks (`symlink`). Owners are inherited from the parent folder,
   which gives root for system paths and `mobile` (501) under `/var/mobile`, as
   dpkg would. Paths with `..` are refused. `/var`, `/etc`, `/tmp` are written
   through `/private/...`.
3. **Maintainer scripts** are not run (they are iOS binaries and shell scripts);
   their effects that matter are reproduced:
   - `firmware-sbin` ships empty placeholders for Apple's `/sbin/fsck`, `mount`, ...
     (its pre/extrainst keep the stock files): the stock files are kept
     (`keep_stock`).
   - Cydia's postinst: `/var/mobile/Library/Cydia`,
     `/var/mobile/Library/Caches/com.saurik.Cydia` (mobile-owned).
   - Cydia's `firmware.sh`: the virtual packages `firmware` 4.2.1, `cy+os.ios`,
     `cy+cpu.arm`, `cy+model.ipod` 2.1, `cy+kernel.darwin` 10.4.0,
     `cy+lib.corefoundation` 550.52, and `/var/lib/cydia/firmware.ver` = 6. (It still
     runs at each boot from `com.saurik.Cydia.Startup` and re-derives them.)
   - **No stashing**: `/.cydia_no_stash`. Stashing moves `/Applications`,
     `/usr/share`, ... to `/var/stash` because a real iPod's system partition is
     small; our rootfs is one 1 GiB volume, so it would only cost time and add
     symlinks. `pam`'s preinst (`move.sh /usr/lib/pam`) is skipped for the same reason.
   - `uikittools`' extrainst rebuilds SpringBoard's app cache; the image has no
     cache yet, SpringBoard builds it (with Cydia) on first boot.
4. **dpkg database**: `/var/lib/dpkg/status` (each control + `Status: install ok
   installed`, `Conffiles:` with md5s, sorted), `info/<pkg>.list` (`/.` + every path
   in the deb), `info/<pkg>.{md5sums,conffiles,preinst,postinst,prerm,postrm,
   extrainst_}`, empty `available`, `updates/`, `triggers/`; APT's
   `/var/lib/apt/lists/partial` and `/var/cache/apt/archives/partial`.

Sources: none are written. On its first launch Cydia sets up its default list
itself (`/var/mobile/Library/Caches/com.saurik.Cydia/sources.list`, with
`/etc/apt/sources.list.d/cydia.list` symlinked to it): `apt.saurik.com ios/550.52`,
BigBoss, ModMyi, ZodTTD and, for iOS 4, `repo666.ultrasn0w.com`, exactly as on a
2011 device. ModMyi, ZodTTD and repo666 no longer exist, so a refresh reports
errors for them; remove them in Sources > Edit.

## Turning it off

- `make sd CYDIA=0` / `make sdimg CYDIA=0` (passes `--no-cydia` to `scripts/mksd.py`)
- `python3 install.py --no-cydia IPSW SD`
- `IOS3DS_CYDIA=0` in the environment (any build path)
- `"cydia": false` in `rootfs/recipe.json`

To remove it from a built image, rebuild with one of the above. To prefetch
(e.g. before going offline): `python3 scripts/cydia.py fetch`.

## Updating the pins

Edit `version`/`path` in `rootfs/cydia.json`, then `python3 scripts/cydia.py pin`
re-downloads each listed deb and rewrites `size` and `sha256`. Check new Mach-Os
still carry an armv6 (or generic ARM) slice.

## Verified in QEMU (re-log 60)

- Build: the hook adds 4-8 s to `mkrootfs` (978 files, ~1460 `hfsplus` calls);
  32 debs + 6 virtual packages in `status`.
- Boot to SpringBoard as before; the Cydia icon is on page 2.
- Cydia launches straight into its UI: no "Preparing Filesystem", no reboot
  request. Installed shows the 32 packages; on first launch Cydia writes its default
  sources.
- At boot, `com.saurik.Cydia.Startup` runs `firmware.sh` in the guest (bash, sed,
  coreutils, gssc): `status` gains the `gsc.*` capability packages.
- With Wi-Fi (`ctr-wifi` model + slirp): Cydia's refresh downloads the
  Telesphoreo (`ios/550.52`) and BigBoss package lists, the Telesphoreo sections list
  appears, and installing `cpio` through Cydia (APT download + dpkg in the guest)
  leaves `cpio 2.9-2p` "install ok installed" with `/usr/bin/cpio`.
- `emu/soak.py --cycles 10` on the image after all of the above.

## Known issues

- **HTTPS pages fail** ("The certificate for this server is invalid"): Cydia's
  home page and package depictions are on https://cydia.saurik.com, whose
  certificate chain iOS 4.2.1's root store can't verify. Lists and .debs come over
  HTTP and work. A newer root store would be a separate change.
- Dead default sources (ModMyi, ZodTTD, repo666) show errors on refresh.
- The startup daemon re-runs `firmware.sh` on every boot (stock behaviour); it
  is a few seconds of background shell work.
- No Cydia Substrate (see above).
