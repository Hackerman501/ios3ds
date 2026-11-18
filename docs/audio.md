# Audio: iOS 4.2.1 sound out of the 3DS speaker

Status: **plays in QEMU, bit-exact; not yet heard on hardware**. On the 3DS
(runs 8-11) every sound failed with `AppleEmbeddedAudioDevice: could not start
DMA: device is not ready`: the I2S controller waits for a GPIO edge nobody
raises (section 3, re-log section 30). With that fixed, an AudioQueue tone goes
through mediaserverd, IOAudio2, the emulated PL080/I2S and the shim into CSND,
and the QEMU board's CSND model records it sample for sample. Volume keys set
the CSND volume (they drive iOS's speaker amp, which the shim translates).

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
| `startDMA` of AppleARMIISAudio (0x80611b48) prints "could not start DMA" when the per-direction start (0x80611474 -> 0x806113c8 -> I2S controller vtable 0x358) fails | 0x80611c54 |
| AppleS5L8900XI2SController DMA start (0x8066d448): with `i2s-version` 0 (no such DT property: version 0, as the boot log says) and frame sync requested, it sets state 1, enables its interrupt, arms a timeout and sleeps until the state is 3 or 4; it returns kIOReturnNotReady unless it reached 3 | 0x8066d490..0x8066d52c |
| That interrupt is GPIO pin 44: i2s0 `interrupts` = 0x2c with `interrupt-parent` = the GPIO controller (phandle 0x0090c9a0). Its action (0x8066caf0) moves state 1 -> 2 on the first edge and 2 -> 3 on the second (then disables itself and wakes the starter); the timeout (0x8066cb84) sets 4 | devicetree.txt, disassembly |
| PL080 channel states: 1 allocated (0x807d08c0), 2 ready (`_initDMAChannel` 0x807d1130, `stopDMACommand` path 0x807d0a38), 3 running (0x807d03c8); `startDMACommand` (vtable 0x354, 0x807d00e4) returns NotReady for state <= 1 | 0x807d0140 |
| Speaker volume is not in the PCM and not in the CS42L58: AppleLM48821Amp's setGain (0x807b8108) maps whole dB to a 5-bit step (0 below -62 dB, 19 = 0 dB, 31 = +18 dB) and writes one byte `step << 3 \| 3` with no sub-address; its mute (0x807b8048) writes `0x04`; its reported range is -38..+18 dB (0x807b800c). The codec's HP output feeds the amp (DT `audio0` `hpout-spkr`) | disassembly, QEMU runs 3-5 |
| iOS writes: media playback at the default volume step 22 (+6 dB), UI sounds step 14 (-10 dB; hardware run10 `val=73`), loudest step 31; the quietest step before mute was 11 (-16 dB) | QEMU runs 3-5 |

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

**Kernel patch** `i2s-start-no-lrck-sync` (same file): at 0x8066d490 the I2S
controller's DMA start enters state 3 (synced) instead of 1 (`mov r3,#1` ->
`mov r3,#3`), so it never waits for the GPIO 44 edges that only an S5L8720
board produces; the interrupt is still disabled and the timeout cancelled as
after a real sync. Without it every output stream fails with kIOReturnNotReady.

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
- Volume: `shim_iic_transfer` recognises the amp's one-byte writes (no
  sub-address): `step << 3 | 3` sets the CSND volume of both channels to
  `10^((dB(step) - 18) / 20)` (table `amp_gain`, step 31 = iOS's loudest =
  unity), `0x04` (the driver's mute) sets 0. `aud_service` applies a change to
  the running CSND channels; `csnd_start` starts with the current value. Being
  digital (before the CODEC), it applies to speaker and headphones alike.
- `aud_cfg_t` / `aud_stat_t` in the shim's `.data` (magic `AUD0cfg!`); mkboot
  writes the fake dmac0/i2s0 page addresses, the ARM9 reads the statistics
  (`amp_step`, `volume` added for the volume mapping).
- I2C log: the first 300 accesses, then only writes that change a register.

**ARM9** (`boot/arm9/audio.c`): CODEC sound init before the jump, and
`audio_poll` in the monitor loop logs stream starts/stops and iOS volume
changes (`[audio] ...` in `sd:/ios3ds/log.txt`). It also polls the headphone
jack (GPIO2 bit 0, debounced over 32 polls) and repeats the init's
`CDC_REG_HEADSET_SEL` routing when it changes (`[audio] headphones plugged
in/unplugged`), only if the CODEC answered at init. iOS keeps its speaker
route either way (no `function-hp_detect`), so its volume steps keep coming
from the amp.

**Test client** `tools/ios3ds-audio/tone` (`/usr/libexec/ios3ds-tone`,
launchd job `com.ios3ds.tone`): plays a test tone through AudioQueue (left =
tone, right = half amplitude) 30 s after launch; default once, 400 ms, 1 kHz,
amplitude 6000. Arguments: `count interval_s ms hz amplitude delay_s`.
AudioToolbox is loaded with dlopen; every failure is logged once
(`tone: ...`) and the program exits. The shim logs `ios3ds-audio: ...` lines (stream start
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
| `kernel/patches-audio.json` | `i2s-start-no-lrck-sync` |
| `rootfs/recipe.json`, `Makefile` | `ios3ds-tone` + its launchd plist; `tools/ios3ds-audio/tone` in `USERLAND` |
| `emu/qemu/hw/arm/nintendo3ds.c`, `emu/build-qemu.sh` | `ctr_csnd_init()` at 0x10103000; build `ctr_csnd.c` |

## 6. QEMU

`emu/qemu/hw/arm/ctr_csnd.c` models CSND (0x10103000) enough to check the
whole path: per channel, CNT bits 15+14 start playback of PCM16/PCM8 from
FCRAM at `67027964 / (0x10000 - SR)` on QEMU's virtual clock, ST_ADDR reads
back the play position (so the shim's resync runs as on hardware), loop mode
wraps to LP_ADDR, one-shot clears bit 15 at the end. Playing channels are
mixed with their VOL halves and MASTER_VOL into a stereo sink that prints
`[csnd]` lines (channel start/stop, every 5 s: frames, non-silent frames,
peak, RMS) and, with `IOS3DS_CSND_WAV=/path.wav` in QEMU's environment,
records the mix. Not modelled: ADPCM, capture, I2S1, the CODEC (the SPI stub
still makes `audio_init` read id 00 and carry on).

```
IOS3DS_CSND_WAV=$PWD/build/out.wav python3 emu/run.py --icount 2 --time 500 \
    --keys "420:w_down,420.3:w_up"        # R = volume up, Q (L) = volume down
```

Use `--icount 2`. Without it QEMU's guest runs about 3x slower than the 3DS
while the virtual (audio) clock runs in real time, so mediaserverd misses its
deadlines and only fragments of a sound reach the ring (run 2: 1,914 of
17,640 frames of a 400 ms tone). With icount every 1 s tone after boot is
complete (44,099 frames), gap-free, 999 Hz, peak 5999/2999 = exactly the
tone's 6000/3000 at unity volume (run 3, re-log section 30).

## 7. Open points / next steps

1. Hardware: listen for the boot tone (`tone: #1 ...` in the log, the shim's
   `ios3ds-audio: tx start` and the ARM9 `[audio] iOS output stream #1
   started`). If the stream starts but nothing is heard: confirm CSND is heard
   with I2S1 DSP volume 32 and that PDN_I2S_CNT/I2S1_CNT are set (the shim
   writes them again from the ARM11).
2. Hardware: the speaker level. iOS's loudest step is CSND unity; the
   CODEC's speaker gains are libn3ds' `hw_cal` defaults. The 3DS volume
   slider is not read.
3. Hardware: headphone jack polling reads GPIO2 bit 0 from the ARM9
   (unverified there; QEMU has no GPIO2 model).
4. Microphone: RX DMA currently delivers silence; CSND capture units could
   feed it.
