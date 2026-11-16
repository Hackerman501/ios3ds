# Audio: iOS 4.2.1 sound out of the 3DS speaker

Status: **implemented, boot-tested in QEMU, not yet run on hardware**. In
QEMU the kexts start (PL080, I2S, CS42L58 with 78 I2C register accesses) and
the home screen renders as before. No output stream has been started yet
(volume/lock key presses did not start one), so the DMA → CSND path is so far
covered only by the host test `tests/test_audio.py`. QEMU has no CSND or
CODEC model, so nothing would be heard there anyway (section 6).

## 1. Summary

```
 iOS userland                 XNU (Apple kexts, unmodified)            ios3ds                      3DS hardware
 ------------                 ------------------------------          ------                      ------------
 app -> AudioToolbox ->       IOAudio2Family                          kernel shim (ARM11, IRQ):
 mediaserverd  ------------>  AppleEmbeddedAudio                      PL080 emulation on the      CSND 0x10103000
                              AppleCS42L58Audio --I2C--> [patched     fake dmac0 page: walks the  ch0 = L, ch1 = R
                              AppleLM48821 amp           transfer ->  LLIs at the I2S word clock, looping PCM16 rings
                                                         shim regfile] copies PCM into the CSND  in FCRAM 0x208C0000
                              AppleARMIISAudio (I2S0, fake page)       rings, raises TC IRQs           |
                              AppleARMPL080DMAC (dmac0, fake page) <-- through the fake VIC (src 16)   v
                                                                                                  I2S1 -> CTR CODEC ->
                              ARM9 monitor, before XNU: CODEC sound init over SPI (audio.c)       speaker / headphones
```

Chosen path: **keep Apple's whole audio stack** and emulate the three pieces
of S5L8720 hardware under it (PL080 DMA controller, I2S0, I2C0 devices), the
same way the CLCD VBlank is emulated. No DSP firmware: the 3DS CSND block plays
PCM straight from FCRAM. Rejected alternatives: a new IOAudio2 driver (needs a
kext built against the 4.2.1 KPIs, far more code); routing through the 3DS DSP
(needs Nintendo's DSP firmware, which is not ours to ship).

## 2. 3DS side (research)

| Block | Address | Who may touch it | Source |
|---|---|---|---|
| CSND (32 channels + 2 capture units) | 0x10103000 | ARM11 (every known driver is ARM11-only; libn3ds has it only under `arm11/`) | libn3ds `include/arm11/drivers/csnd.h`, `source/arm11/drivers/csnd.c` |
| I2S1_CNT / I2S2_CNT | 0x10145000 / 0x10145002 (u16) | ARM11 in libn3ds/fastboot3DS; ARM9 unverified | libn3ds `include/arm11/drivers/i2s.h`; fastboot3DS `source/arm11/hardware/codec.c` |
| PDN_I2S_CNT (CODEC MCLK) | 0x10141220 (u8, bit 1 = MCLK2) | ARM11 in libn3ds; Luma's ARM9 uses other PDN registers (`PDN_GPU_CNT` 0x10141200) | libn3ds `include/arm11/drivers/pdn.h`, codec.c `pdnControlMclk` |
| CTR CODEC | NSPI bus 2 (0x10142800), CS0 | both CPUs (ios3ds' ARM9 already reads the touch ADC through it) | libn3ds `codec.c`, `codec_regmap.h`; `boot/arm9/tsc.c` |
| Headphone jack | GPIO2 bit 0 (0x10147010) | both | libn3ds `gpio.h` `GPIO_2_HEADPH_JACK` |

CSND facts used (libn3ds csnd.h): channel registers at `0x400 + 0x20*n`:
CNT (u16: bit15 start, bit14 playing / unpause, bits 12-13 format 1 = PCM16,
bits 10-11 repeat 1 = loop, bit 6 linear interpolation), SR (u16,
`0x10000 - 67027964/Fs`), VOL (u32, right in the low half, left in the high
half, 0..0x8000), CAPVOL, ST_ADDR (physical start address, **reads back the
play position**), SIZE (bytes), LP_ADDR (loop restart). Global: MASTER_VOL
(0x000), CNT (0x002, bit 15 enable, bit 14 resample filter). CSND mixes all
channels and resamples them to the I2S1 rate; the I2S1 low 6 bits are the
DSP-side master volume, which libn3ds/fastboot3DS leave at 0 (they only play
GBA/DS audio through the legacy volume, bits 6-11). We set both to 32 (100 %);
whether CSND is scaled by the "DSP" volume is the first thing to check on
hardware.

CODEC sound init = libn3ds `CODEC_init()` + `soundInit()` without microphone,
shutter-sound, HWCAL filter coefficients and the o3DS de-pop GPIO, using the
`hw_cal.c` default gains (speaker +12 dB driver / -3.5 dB analog, headphones
0 dB). I2S1 runs at 47605 Hz (CODEC NDAC 0x85) instead of libn3ds' 32 kHz.
Done by the ARM9 (`boot/arm9/audio.c`) before XNU starts, because the ARM9
owns the SPI bus; the ARM11 shim writes PDN_I2S_CNT and I2S1_CNT again when it
starts CSND, in case the ARM9 cannot reach those registers.

## 3. iOS side (RE, kernelcache 8C148)

| Fact | Where |
|---|---|
| DT `/arm-io/dmac0`: `dmac,pl080`, reg child 0x00200000, interrupt 0x10 (VIC0 source 16) | `build/staging/devicetree.txt` |
| DT `/arm-io/i2s0` `dma-channels`: TX cfg 0x0A80 ctl 0x03249000 FIFO 0x3CA00010; RX cfg 0x1056 FIFO 0x3CA00038 (16-bit, burst 4) | devicetree.txt |
| DT `/arm-io/i2c0/audio0` (CS42L58 @0x4A) and `/spkr-amp` (LM48821 @0x76); `/arm-io/i2s0/audio0` = data side | devicetree.txt |
| `AppleARMIICDevice` transfer (vtable 0x35c) = 0x802e4d1e (Thumb): `(this, op, sublen, sub*, len, buf*)`, len/buf on the stack, returns 0xE00002C2 for missing args | disassembly |
| PL080 driver start: `IOFilterInterruptEventSource` at 0x807d1574, filter = vtable 0x370 → 0x807d115c, action = vtable 0x36c → 0x807d122c | disassembly |
| Filter reads the TC status once (vfunc 0x3a4), writes it to IntTCClear once (vfunc 0x3a8), loops over the bits; **no re-read loop**, so write-1-to-clear can be applied lazily by the emulator | 0x807d115c..0x807d1228 |
| LLIs: 16-byte items in a driver-owned page, ≤ 0xE00 transfers each, I bit on the last item of a ring command; channels stay enabled across chained commands | 0x807d1ba4.., 0x807d01c4 |
| `AppleARMFunction` lookups `waitForService` with no timeout, so DT `function-*` pointing at a deleted node hangs `start` | AppleARMPlatform 0x802e4698 |
| Without `function-hp_detect` AppleEmbeddedAudio skips headphone detection and stays on the speaker route | 0x806533a4..c0 |

## 4. What ios3ds changes

**Device tree** (`tools/ios3ds-audio/dt_audio.py`, imported by `kernel/dtgen.py`):
keep `dmac0`, `i2c0`, `i2s0` (previously deleted), each on a fake register page
(`FAKEIO`); delete `i2c0/accelerometer`, `i2c0/pmu`, `i2c0/tethered` (drivers
that would make decisions from an I2C bus that answers with zeros); drop
`audio0`'s `function-hp_detect`, `mikey`, `interrupts`, `interrupt-parent`
(they point at deleted nodes). GPIO functions (codec reset, amp enable,
I2C SDA/SCL) stay: the GPIO block is already a fake page.

**Kernel patch** (`kernel/patches-audio.json`, picked up by `kernel/patcher.py`):
`iic-device-transfer` → `shim+0x24` → `shim_iic_transfer`: one 256-byte
register file per I2C device (writes stored, reads return them; CS42L58 MAP
auto-increment bit masked). The I2C controller itself is never used.

**Kernel shim** (`kernel/shim/audio.c`, `audio_glue.S`, `audio.h`):

- `shim_irq` (`entry.S`) jumps to `shim_audio_hook` right after the GIC ack.
  On the private timer (29) and VBlank (42) it runs `aud_service(now)`:
  - applies pending IntTCClear/IntErrClear writes (the page is plain RAM);
  - advances the I2S word clock: `words = dt * 128 / 1520` (shim time base
    134055928/128 Hz, CSND rate 67027964/1520 = 44097 Hz, same crystal: no drift);
  - walks every enabled channel: memory→I2S TX is consumed at that clock and
    de-interleaved into the CSND rings (L, R, 8192 frames each, written 2048
    frames ahead of CSND); I2S RX gets silence at the same clock; other
    memory→peripheral transfers complete at once; anything else waits;
  - SrcAddr/DestAddr/LLI/Control advance exactly like the real controller;
    TC bits are raised for items with the I bit (and ITC set), EnbldChns kept up.
- A raised TC is delivered like the CLCD frame IRQ: VICVECTADDR16 → VICADDRESS,
  then XNU's IRQ path; on VBlank the private timer is re-armed 2 ticks out so
  the CLCD injection is not lost.
- `shim_set_decrementer` (`entry.S`) calls `shim_audio_setdec`: the private
  timer is shortened to the end of the running LLI item when that comes first,
  so TC interrupts arrive on time; the early wake is swallowed and XNU's own
  deadline re-armed.
- CSND: started at the first TX stream, two looping PCM16 channels, both
  unpaused back to back; the write pointer is re-aligned when CSND's position
  register (once it is seen moving) drifts outside [1/16, 1/2] of the ring;
  after 3 s of silence CSND is stopped. 1 MiB device section 0x10100000 →
  VA 0xBFC00000 is added on first use.
- `aud_cfg_t` / `aud_stat_t` in the shim's `.data` (magic `AUD0cfg!`); mkboot
  writes the fake dmac0/i2s0 page addresses, the ARM9 reads the statistics.

**ARM9** (`boot/arm9/audio.c`): CODEC sound init before the jump, and
`audio_poll` in the monitor loop logs stream starts/stops (`[audio] ...` in
`sd:/ios3ds/log.txt`). The shim logs `ios3ds-audio: ...` lines (stream start
with the first LLI, I2C traffic) into the XNU log ring.

Memory: CSND rings at PA 0x208C0000..0x208C8000, between the XNU log ring and
`topOfKernelData` (mapped by XNU's static map, never given to the VM).

## 5. Shared-file hooks

| File | Change |
|---|---|
| `kernel/dtgen.py` | import `dt_audio`; extend `DELETE`/`FAKEIO`; call `dt_audio.apply(root)` |
| `kernel/shim/entry.S` | `+0x24 b shim_iic_transfer`; `bl shim_audio_setdec` in `shim_set_decrementer`; `b shim_audio_hook` + `shim_irq_resume` label in `shim_irq` |
| `scripts/mkboot.py` | build `kernel/shim/audio*.[cS]`; write dmac0/i2s0 page PAs into `aud_cfg_t` |
| `boot/arm9/main.c`, `boot/Makefile` | `audio_init()` before the jump, `audio_poll()` in the monitor loop; `arm9/audio.c` |
| `boot/arm9/tsc.c`, `tsc.h` | CODEC SPI helpers made public, bus init split out (`cdc_bus_init`) |

## 6. QEMU

The `nintendo3ds` QEMU board has no CSND, I2S or CODEC model: writes go to
unimplemented IO and reads return 0, so CSND's position never moves (the shim
then never resyncs) and nothing is audible; the SPI stub makes `audio_init`
log "no CODEC". Everything above CSND (kexts, DT, I2C register files, PL080
emulation, VIC injection) runs as on hardware. A minimal CSND model for the
board would be: a 0x1000-byte register page at 0x10103000; per channel, when
CNT bit 15 and bit 14 are set, advance ST_ADDR by `2 * Fs * dt` bytes
(`Fs = 67027964 / (0x10000 - SR)`) through `[LP_ADDR, ST_ADDR0 + SIZE)` in
loop mode, clear bit 15 at the end in one-shot mode; optionally mix the PCM16
samples (VOL halves as gains) into a QEMU audio backend at the I2S1 rate.

## 7. Open points / next steps

1. Hardware: confirm CSND output is audible with I2S1 DSP volume 32 and that
   the ARM9 may write PDN_I2S_CNT/I2S1_CNT (else the shim's writes cover it).
2. Volume keys / mute switch: iOS sets the CS42L58 volume over I2C into the
   register file; mapping those registers (or the CS42L58 master volume) onto
   CSND channel volume is not done yet.
3. Headphone switching at run time (GPIO2 IRQ) and a real
   `function-hp_detect`.
4. Microphone: RX DMA currently delivers silence; CSND capture units could
   feed it.
5. Start a real stream in QEMU: a small userland tool (AudioQueue or
   `AudioServicesPlaySystemSound`) under `tools/ios3ds-audio/`, then look for
   the shim's `ios3ds-audio: tx start` line and the ARM9 `[audio] stream` lines.
   Volume keys gave no HUD in QEMU; find out why (no route? mediaserverd waiting
   on something?).
