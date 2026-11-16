# Running ios3ds on a real Old 3DS (without risking the existing setup)

**Goal:** test on your own console with boot9strap and Luma3DS, while leaving everything that is already installed (Luma, boot9strap, homebrew, NAND) untouched and bootable.

## 1. Why this can't brick the console

| Risk | Status | How it is ensured / checked |
|---|---|---|
| Writing NAND (eMMC) | **never** | `boot/arm9/sdmmc.c` has no eMMC code at all (removed when imported); the only device ever selected is `devicenumber = 0` (SD, `REG_SDPORTSEL` port 0). XNU has no driver for the 3DS eMMC, and the TMIO controller at 0x10006000 is only reachable from the ARM9. |
| Modifying FIRM0/FIRM1, boot9strap, Luma | **never** | Nothing writes to NAND (see above). ios3ds is just a payload file on SD that Luma chainloads into RAM. |
| Persistent chip state (MCU, PMIC, CODEC) | none written | No I2C/MCU writes anywhere. The touch driver configures CODEC registers over SPI; those reset at power-off. |
| SD card damage | possible, limited | The ARM9 writes only `sd:/ios3ds/log.txt`, `log_prev.txt` and `rootfs.hfs` (iOS's disk image), through FatFs. A power loss during a write can at worst corrupt that FAT volume, as with any homebrew. Mitigation: back up the SD card (or use a spare card). |
| Hang / black screen | recoverable | Hold POWER for ~10 s to force the console off. ios3ds runs entirely from RAM, so the next normal boot is your usual Luma + HOME Menu. |
| Battery / heat | low | Same as any long-running homebrew: no clock or voltage changes. |

There is no software path back into NAND. The worst realistic outcome is a corrupted SD card, which you restore from your backup.

## 2. Before the first run (one time)

1. **Back up the SD card:** copy all of it to your PC.
2. **Back up the NAND, if you don't have a backup yet.** If you followed the 3ds.hacks.guide "Finalizing Setup" page, you already have one in `sd:/gm9/out/` (`<date>_<serial>_sysnand_###.bin` plus `essential.exefs`). Otherwise use GodMode9's NAND backup option (`[S:] SYSNAND VIRTUAL` → `nand.bin` → A → *NAND image options* → *Backup SysNAND*). Copy the backup to your PC. ios3ds never needs them; they are your safety net for anything else.
3. **Check Luma's version:** Luma3DS 10+ with boot9strap. The chainloader is the menu you get by holding START during boot.
4. Build: `make sd`, then copy the **contents** of `build/sd/` to the SD root:
   - `luma/payloads/ios3ds.firm` (≈200 KB);
   - `ios3ds/` (kernel, device tree, shim, `boot.cfg`, `rootfs.hfs` ≈1 GB).

   Nothing existing is overwritten except an older `ios3ds/`.
5. Optional quick-launch: rename the payload to `x_ios3ds.firm`. Luma then boots it directly when **X** is held at power-on; all other boots are unchanged.

## 3. Test stages

Every stage writes to the screen and to `sd:/ios3ds/log.txt`. The previous run is kept as `log_prev.txt`. After each stage, power off and send `log.txt`.

| Stage | How | Expected | If it fails |
|---|---|---|---|
| 0: monitor only | Boot ios3ds, hold **B** | Bottom screen: version, "SD ok", "ARM11 ok"; the monitor stays in its menu | Screen/SD/ARM11 bring-up problem: the log says which step |
| 1: kernel start | Boot without B | XNU's first lines on the top screen ("Darwin Kernel Version 10.4.0…") | Look at the last log line: device tree, boot_args, MMU |
| 2: root disk | – | `BSD root: md0`, launchd, daemons in the log | Block server: TMIO timing on real SD |
| 3: SpringBoard | – | Home screen on the top screen (turn the console 90° clockwise) | `[smp]` thread dumps in the log; SELECT+START for a panic dump |
| 4: input | START = Home, SELECT = lock, L/R = volume, touch on the bottom screen | Spotlight opens on Home; touches move or tap | Log lines `[input] …` and `tsc_ok` |

To get back: hold POWER ~10 s, then power on normally.

## 4. Can it run as a `.3dsx`?

A `.3dsx` is a user-mode program under Nintendo's kernel. XNU needs to own the CPU (MMU, vectors, interrupts) and our design needs the ARM9 as its I/O processor, so XNU cannot run *inside* a `.3dsx`. A `.3dsx` can, however, act as a **launcher** that hands the machine over to `ios3ds.firm`, with no START-at-boot menu. Return to Horizon by rebooting. What is feasible on current Luma3DS is being researched; see section 5 (to be filled in from the research agent's findings).

## 5. 3DSX launcher (research)

_Pending._
