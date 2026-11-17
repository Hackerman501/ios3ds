# Handoff (2026-11-17)

Where ios3ds stands and what to do next. Read [README.md](README.md) and [docs/architecture.md](docs/architecture.md) first; [docs/re-log.md](docs/re-log.md) has every reverse-engineering step.

## 1. Status

| Area | QEMU board | Real Old 3DS |
|---|---|---|
| Monitor, SD, ARM11 bring-up, kernel load | works | works |
| XNU early boot | to SpringBoard home screen | **hangs right after `iBoot version: ios3ds-loader`** |
| Buttons (Home, lock, volume via GraphicsServices) | works | not reached |
| Touch (GS hand events, `flipx`) | works | not reached |
| Screen modes (X: top / both / bottom) | works | works in the monitor |
| GL ES 1.1 software engine (`tools/ios3ds-gl`), gltest | PASS | not reached |
| Wi-Fi (ARM9 AR6014 + `ios3ds-wifid`) | off cleanly without `wifi.cfg` | not reached |
| Audio (shim PL080/I2S/I2C emulation → CSND) | kexts start | CODEC answers (id 11 rev 16) |
| `.3dsx` launcher (rename swap + restore) | — | works (`launcher swap: restored`) |

## 2. The current problem: hardware hang after "iBoot version"

Hardware logs and photos are in `build/hwlogs/`. That directory isn't in git; copy it over if you need it.

Facts, from the latest probe build `cc0ccbd`:

| Probe (monitor rows 20–26) | Value | Meaning |
|---|---|---|
| `sctlr` | `00c5787d` | MMU, caches, high vectors (V=1), XP=1 |
| `irq` | `0` | **the shim's IRQ entry was never reached** |
| `pt` count / ctrl / status | `1c29` / `7f05` / `0` | private timer counting down (one-shot, IT enabled, 3264 of 10473 ticks gone) |
| GIC `cc pm hp dc act` | `1 f0 3ff 1 0` | CPU interface and distributor on, priority mask 0xF0, nothing pending or active |
| `en` / `pd` | `2000ffff/00000400` / `0/0` | IRQ 29 (timer) and 42 (VBlank) enabled, nothing pending yet |
| `abs` | 34 reads, last raw `0xcc4`, CPSR `0x93` | all 34 clock reads happen within ~3 ms; the last one has IRQs masked |
| `dec` | 1 call, 10473 ticks, caller `0x800695d3` | one 10 ms deadline armed |
| `shim` | `i1 t0 o0 L0 d0 v0 j0` | no timer IRQs, no other IRQs, no VBlanks |

The snapshot is taken at each clock read, so it shows the state just before the hang. The GIC and timer are configured correctly at that point.

In QEMU, read 35 comes from a new thread with IRQs enabled, and by then `dec` = 4: timer IRQs have fired. On hardware, read 35 never happens and no IRQ is ever taken.

Two hypotheses remain:
- **A:** XNU enables IRQs and idles; the timer fires, but the IRQ never reaches `shim_irq`. Possible causes: the vector page at 0xFFFF0000 (page-table or cache coherency issue that QEMU doesn't model), the vector literal, or the patched `fleh_irq` at 0x8006648c.
- **B:** XNU hangs between read 34 and the point where it enables IRQs: context switch, an MPCore vs ARM1176 instruction difference, VFP, or a spin with IRQs masked.

Already ruled out:
- an interrupt storm from leftover Luma IRQs (all other IRQs are disabled and cleared in `shim_init`);
- AMP mode (the ARM11 stub now enables SCU+SMP, ACTLR reads `0x2f` on hardware).

Neither changed the hang.

**What was in progress:** `emu/gdb/t34.gdb` (run from the repo root): a gdb trace in QEMU that single-steps from read 34 until CPSR.I clears. It shows exactly which code runs on the way to "IRQs on", so that code can be checked for MPCore differences. The script is shown below; QEMU was started with `-s -S`.

```
# t34.gdb
set pagination off
target extended-remote :1234
thread 1.1
hbreak *0x80840010 thread 1.1
condition 1 *(unsigned*)0x80840080 == 33
continue
delete
set scheduler-locking step
source emu/gdb/trace_irq_on.py   # loop: stepi, log pc/cpsr, stop when (cpsr & 0x80) == 0
```

Start QEMU with:

```
build/qemu/b/qemu-system-arm -M nintendo3ds -kernel build/boot/ios3ds.firm \
  -drive file=build/sd.img,if=sd,format=raw -display none -serial none -s -S
```

The gdb binary is `arm-none-eabi-gdb` from arm-gnu-toolchain 14.2. Inferior 1 is the ARM11; attach inferior 2 (the ARM9) and use `set schedule-multiple on` if you need both.

**Next probe build, to settle A vs B:**
1. **ARM11 snapshot** (in `shim_get_abstime`): add TTBR0, TTBCR, DACR and MPIDR. Also read, through the kernel's linear map, the L1 entry for 0xFFF00000, its L2 entry for 0xFFFF0000, and the vector words at +0x18 and the literal they load.
2. **ARM9, after the hang:** walk the same tables in FCRAM (physical memory) and show them. If the ARM11's cached view differs from the ARM9's memory view, the hardware walker or the IRQ fetch sees stale data, which QEMU can't reproduce.
3. If the vectors check out: patch a counter at the instruction XNU uses to enable IRQs (found by the trace above). That tells whether it is reached (B vs A).

## 3. Hardware workflow

- **SD card reader:** `devilapi@192.168.178.83` (SSH key auth); the card mounts at `/run/media/devilapi/3ds`.
- **Before copying a new build**, save the old log:

  ```
  scp devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/log.txt build/hwlogs/runN.txt
  ```
- **Copy the build.** `rootfs.hfs` (1 GB) is already on the card; only re-copy it when the rootfs changed:

  ```
  rsync -rt --modify-window=1 --no-perms --exclude rootfs.hfs build/sd/ devilapi@192.168.178.83:/run/media/devilapi/3ds/
  ssh devilapi@192.168.178.83 sync
  ```
- **On the 3DS:** launch from the Homebrew Launcher (`ios3ds.3dsx`), or Luma chainloader → `ios3ds.firm`. Probe values appear on the bottom screen, rows 20–29.
- **Safety rules:**
  - nothing ever writes NAND or FIRM;
  - Apple files are never committed (everything is generated from the IPSW at build time);
  - every build shows its version string on screen.

  See [docs/real-hardware.md](docs/real-hardware.md).

## 4. Building on a new machine

- Put the iPod2,1 4.2.1 IPSW where the README says, then run `make sd`. That builds the firm, kernel, shim, rootfs and `build/sd/`. `make test` runs the unit tests.
- **QEMU:** the patched tree is `emu/qemu`; it builds to `build/qemu/b/qemu-system-arm`. See [docs/emulator.md](docs/emulator.md).
- **Viewer:** `emu/viewer.py` (Tk + QMP). Headless touch test: `emu/tap.py`.
- **3DS launcher:** needs devkitARM + libctru (here taken from `../rockbox-3ds/scripts/env.sh`). Then run `make -C tools/ios3ds-launcher`.

## 5. Branches

| Branch | Status |
|---|---|
| `main` | everything merged (GL software engine, Wi-Fi, audio, input, screen modes, launcher, HW probes) |
| `agent/pica` | **not merged on purpose.** PICA200 GL back end: GPU service in the shim, GX model in QEMU, `IOS3DS_GL=pica\|auto`, `-ffixed-r9` in mkboot. Merge once the hardware boot works. Pushed to origin. |

## 6. Backlog after the hang is fixed

1. Real hardware, stages 2–4 in [docs/real-hardware.md](docs/real-hardware.md): root disk over real SD timing, SpringBoard, input.
2. Merge `agent/pica`, then test PICA200 on hardware.
3. Wi-Fi on hardware: `sd:/ios3ds/wifi.cfg` (SSID/PSK) and the NWM firmware extraction ([docs/wifi.md](docs/wifi.md)).
4. Audio on hardware: speaker and headphones through CSND ([docs/audio.md](docs/audio.md)).
