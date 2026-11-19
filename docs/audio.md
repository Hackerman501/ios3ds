# Audio: iOS 4.2.1 sound out of the 3DS speaker

Status: **output plays in QEMU, bit-exact; microphone recording works in QEMU (section 7); neither verified on hardware yet**. On the 3DS
(runs 8-11) every sound failed with `AppleEmbeddedAudioDevice: could not start
DMA: device is not ready`: the I2S controller waits for a GPIO edge nobody
raises (section 3, re-log section 32). With that fixed, an AudioQueue tone goes
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
| With it, AppleEmbeddedAudio reads the jack through the function (0x8065117c: non-zero = headphones) and re-reads it on `audio0`'s interrupt 0 (IOInterruptEventSource, 0x8065330c). A GPIO function `<gpio phandle, 'GPIO', pin, flags>` reads port `pin >> 8` bit `pin & 7` at page + port * 0x20 + 4 (AppleS5L8900XGPIO 0x8066db60), flags byte 1 = 1: not inverted (0x8066dcc4) | disassembly, QEMU re-log 57 |
| On the headphone route iOS shuts the speaker amp down (`0x04`), plays the PCM unscaled and sets the volume on the CS42L58: reg 0x1a, 1 dB steps, bits 0-6 = dB as 7-bit two's complement (0x44 = -60 dB, 0x6c = -20 dB, 0x02 = +2 dB at full volume), bit 7 = mute (0x80, written around route changes); the driver's range table (0x8065fb38) and clamp (0x8065f3a4) use the same -60 dB, 1 dB steps | QEMU re-log 57, disassembly |
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
`audio0`'s `mikey` (it points at a deleted node) and rebuild its
`function-hp_detect`, `interrupts`, `interrupt-parent` on what the 3DS has:
pin 0x0d00 of the fake GPIO page (data word +0x1a4, bit 0, 1 = headphones;
no other function uses port 13) and VIC0 source 12 (unassigned). GPIO
functions (codec reset, amp enable, I2C SDA/SCL) stay: the GPIO block is
already a fake page.

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
  `10^(dB(step) / 20)` relative to step 19 = 0 dB = unity (table `amp_gain`,
  `AUD_AMP_REF_STEP`; louder steps clamp), `0x04` (the driver's mute) sets 0.
  Since the N81 routing (section 7) iOS no longer changes the amp with the
  volume: it writes a fixed step 19 for media (13 for UI sounds) at stream
  start and applies the volume (keys, slider) to the PCM itself (re-log 54).
  `aud_service` applies a change to
  the running CSND channels; `csnd_start` starts with the current value.
  On iOS's headphone route the amp is off and the volume comes from the
  CS42L58's reg 0x1a instead (section 3): `10^((dB - 2) / 20)`, +2 dB (iOS's
  full volume) = unity, bit 7 = 0 (`hp_gain`, `AUD_HPVOL_REF_DB`), so full
  volume is full scale on both outputs. The CODEC's own speaker/headphone
  gains (ARM9) stay as libn3ds sets them.
- Headphones (`hp_service`, run from `aud_service`): the ARM9 publishes the
  debounced jack in `aud_mic_t.jack` (or `hp_disabled`); the shim mirrors it
  into the fake GPIO detect pin and, once AppleEmbeddedAudio has programmed
  VICVECTADDR12, delivers the jack interrupt like the DMA one (after it, one
  vector per wake). `hp_route=0` / an unreliable jack: iOS is told
  "unplugged" once and never again (`hp_state` = `AUD_HP_OFF`). Logs
  `ios3ds-audio: headphones in|out, jack interrupt`.
- `aud_cfg_t` / `aud_stat_t` in the shim's `.data` (magic `AUD0cfg!`); mkboot
  writes the fake dmac0/i2s0 page addresses, the ARM9 reads the statistics
  (`amp_step`, `volume` added for the volume mapping).
- I2C log: the first 300 accesses, then only writes that change a register.

**ARM9** (`boot/arm9/audio.c`): CODEC sound init before the jump, and
`audio_poll` in the monitor loop logs stream starts/stops and iOS volume
changes (`[audio] ...` in `sd:/ios3ds/log.txt`). It also polls the headphone
jack (GPIO2 bit 0, debounced over 32 polls), repeats the init's
`CDC_REG_HEADSET_SEL` routing when it changes (only if the CODEC answered at
init) and tells iOS through the shim (`boot.cfg hp_route=1`, the default):
`[audio] headphones plugged in: CODEC output -> headphones, iOS route ->
headphones (CODEC headset detect XX)`, then `[audio] iOS route -> ...` and
`[audio] iOS headphone volume: CS42L58 XX (N dB) -> CSND volume ...`.
Fail-safe for what hardware has not confirmed (GPIO2 bit 0 = jack, 1 =
plugged): if the bit reads "plugged" at boot, iOS stays on the speaker route
until it has read "unplugged" once (one `[audio] jack: ...` warning); more
than 8 changes in 10 s and iOS stays on the speaker route until reboot (one
warning). `hp_route=0` keeps iOS on the speaker route; the CODEC still
follows the jack.

**Test client** `tools/ios3ds-audio/tone` (`/usr/libexec/ios3ds-tone`,
launchd job `com.ios3ds.tone`): plays a test tone through AudioQueue (left =
tone, right = half amplitude) 30 s after launch; default once, 400 ms, 1 kHz,
amplitude 6000. Arguments: `count interval_s ms hz amplitude delay_s`.
AudioToolbox is loaded with dlopen; every failure is logged once
(`tone: ...`) and the program exits. Each tone logs iOS's route (`route "Speaker"` /
`"Headphone"`); route changes are logged with their reason (`tone: route
change, reason 2 (old device unavailable): "Headphone" -> "Speaker"`), and
reason 2 pauses the playing tone as the iPod app pauses on unplug. The shim logs `ios3ds-audio: ...` lines (stream start
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
| `emu/qemu/hw/arm/nintendo3ds.c`, `emu/build-qemu.sh` | `ctr_csnd_init()` at 0x10103000, `ctr_mic_init()` at 0x10162000; build `ctr_csnd.c` |
| `boot/arm9/main.c` | `boot.cfg mic=` key, `audio_set_mic()` |
| `rootfs/recipe.json` | VirtualAudio product-table patch (section 7) |

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

The headphone jack is GPIO2 bit 0 (a 4-byte window over the Wi-Fi model's
GPIO block) behind the machine property `headphones`:

```
python3 emu/run.py --icount 2 --time 300 --shots 168 \
    --hmp "150=qom-set /machine headphones on;165=qom-set /machine mcu-volume 56;200=qom-set /machine headphones off"
```

QEMU prints `[jack] headphones plugged in|unplugged`; the volume HUD then
says "headphones" (re-log 57).

```
IOS3DS_CSND_WAV=$PWD/build/out.wav python3 emu/run.py --icount 2 --time 500 \
    --keys "420:w_down,420.3:w_up"        # R = volume up, Q (L) = volume down
```

Use `--icount 2`. Without it QEMU's guest runs about 3x slower than the 3DS
while the virtual (audio) clock runs in real time, so mediaserverd misses its
deadlines and only fragments of a sound reach the ring (run 2: 1,914 of
17,640 frames of a 400 ms tone). With icount every 1 s tone after boot is
complete (44,099 frames), gap-free, 999 Hz, peak 5999/2999 = exactly the
tone's 6000/3000 at unity volume (run 3, re-log section 32).

## 7. Microphone (re-log section 49)

```
 iOS: AudioQueue input -> mediaserverd (VirtualAudio HAL plug-in: route SpeakerAndMicrophone)
      -> IOAudio2 input stream -> AppleEmbeddedAudio -> PL080 RX channel (I2S RX FIFO 0x3CA00038)
 shim (ARM11 IRQ, every 1 ms while iOS records): MIC FIFO 0x10162004 -> mic ring (FCRAM 0x208C8000, aud_mic_t)
                   RX words = ring samples, linearly interpolated 11.9 kHz -> 44.1 kHz, scaled, L = R
 ARM9 monitor loop: CODEC ADC powered while aud_mic_t.want is set (SPI is the ARM9's)
 3DS: built-in mic -> CODEC ADC (I2S2 47.6 kHz) -> MIC block (rate / 4)
```

**Why iOS saw no input.** An iPod2,1 (N72) has no built-in microphone; it
records only from a wired headset. AudioSession reported input available 0,
route "", and `AudioSessionSetActive`/`AudioQueueNewInput` failed with `'nopi'`.
Two things decide it:

| Fact | Where |
|---|---|
| AppleCS42L58Audio offers input data sources from DT properties of `audio0`: `line` always, `imic` with `ain2b-intmic`, `emic` with `ain2a-extmic`; AppleEmbeddedAudio names them "Line in" / "Internal microphone" / "External microphone" | 0x80660294, 0x80655a04 |
| Routing is done by mediaserverd's HAL plug-in `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` (AudioDriverPlugIns-252.1, stripped). It picks a per-product routing database (`RoutingSettings_N72`, `_N81`, `_N82`, ...) by matching `hw.model` against the table at file offset 0x287210 (`K48 K93 K94 K95 K66 N18 N72 N81 N82 N88 N90 N92 N94`, 4 bytes each). N72's database has no built-in mic port | strings, table |
| CoreMedia maps the plug-in's ports to routes: `MicrophoneBuiltIn`, `MicrophoneWired`, `LineIn`; `SpeakerAndMicrophone` = speaker out + built-in mic | CoreMedia 0x3743534c |

**What ios3ds changes for it.**

- `dt_audio.py` adds `ain2b-intmic` to `audio0`, so the codec has an internal mic data source.
- `rootfs/recipe.json` patches the plug-in's product table: the N81 slot (iPod touch 4G: speaker, headphones, built-in mic, no receiver) reads `N72`, and the N72 slot `X72`. iOS then reports input available 1 with route `SpeakerAndMicrophone`. Output stays on the speaker, and volume still comes as amp steps.
- **Shim** (`kernel/shim/audio.c` `mic_want`/`mic_drain`/`mic_fill`):
  - When iOS's input DMA starts, the shim enables the MIC block at I2S2 / 4 = 11,901 Hz.
  - On every service it drains the 16-word FIFO (2.7 ms) into the ring. While an input stream runs the shim services every 1 ms (`RX_STEP_TICKS`); without that the HAL read the newest input before it was written.
  - The RX emulation reads the ring instead of writing zeros. It interpolates linearly to the I2S frame rate, sends the same value to L and R, and trails the writer by 20 ms. An empty ring repeats the last value; a reader that fell more than half a ring behind jumps (`mic_underruns`, `mic_resyncs`).
  - Which word of a frame (L or R) is being written comes from the destination address (frames are 4 bytes), not from the previous stream. Carrying the phase over put an odd-length stream's successor half a frame off, and iOS's input chain turned that into spikes; it showed as recordings that were bit-identically good or bad depending on the stream before.
  - The `rx stop` log line reports frame-to-frame jumps above 16 in the delivered input, the largest one, underruns and resyncs.
  - The samples are scaled by `AUD_MIC_GAIN_Q15` (−23.65 dB). iOS's input chain for this route adds a fixed +23.65 dB (×15.23, measured: a 1000-amplitude sine came back at 15,232, an 8000 one clipped). With the compensation, apps get the 3DS mic at the level the CODEC's PGA gives it.
  - The mic stays on until the input stream has been gone for 1 s (`AUD_MIC_HOLD_TICKS`). The RX channel can be idle for a moment between two queued commands, and switching off there punched 50 ms holes into a recording.
  - Safety, each case logged once (`ios3ds-audio: mic: ... input stays silent`; ring state FAILED, reason in `why`), then silence: no sample within 250 ms, a FIFO that never reports empty (> 64 words in one service), the block disabling itself, or no IO window.
  - Draining from the ARM11 is deliberate. The 3DS's own mic service runs there, while ARM9 access to 0x10162000 is unverified. A first version drained from the ARM9 monitor loop and overran the FIFO constantly in QEMU.
- **ARM9** (`boot/arm9/audio.c` `mic_poll`):
  - At boot, `audio_init` applies the CODEC side of libn3ds `microphoneInit` (ADC select, input impedance, bias, PGA, without the calibration filters).
  - While `aud_mic_t.want` is set, it powers and unmutes the CODEC's ADC (page 0 registers 81/82).
  - It logs the shim's mic state changes (`[audio] mic on/off/...`).
  - `boot.cfg mic=0` sets `disabled` in its own cache line, and the shim then never touches the MIC block.
- **QEMU:** `ctr_mic_init` (in `ctr_csnd.c`) models the MIC block (GBATEK layout).
  - Its source is set with `IOS3DS_MIC=ramp|sine:HZ:AMP|file.wav`.
  - `IOS3DS_MIC_FIFO=n` enlarges the FIFO, so a slow emulated ARM9 does not hide other problems.
- **Test client:** `ios3ds-tone rec count interval ms delay [dir|- [plar|reca]]` logs input availability, route, frames, peak, RMS and a least-squares sine fit. With `dir` it also saves `recN.wav`, which ends up in `rootfs.hfs` on the SD image (md0 writes go through); category `reca` is RecordAudio, as Voice Memos uses. The shipped job never records.
- **Evidence (QEMU, `--icount 2`, `IOS3DS_MIC=sine:440:1000`, real 32-sample FIFO):**
  - Every recording, in both PlayAndRecord and RecordAudio, fits 440.03 Hz at amplitude 1003.0 (level within 0.3 % of the source) with the residual 41 dB below the signal (interpolation images plus iOS's filters).
  - 0 FIFO overruns, 0 underruns, 0 resyncs, about 1,050 shim services a second while the mic is on.

**iOS processes the input.** The VAD's input chain for this route
high-passes the mic and applies the fixed gain above. A 16-bit counter comes
back as near-zero values with a decaying start transient. So the app-level
recording cannot be bit-exact. Bit-exactness is shown below iOS: the host
test feeds a counter through the RX emulation and checks every delivered frame
(steps of 0 or +1, L = R, the expected number of new samples, no underruns).

## 8. Open points / next steps

1. Hardware (boot.cfg `diag=1`, docs/boot-perf.md §3): listen for the boot tone (`tone: #1 ...` in the log, the shim's
   `ios3ds-audio: tx start` and the ARM9 `[audio] iOS output stream #1
   started`). If the stream starts but nothing is heard: confirm CSND is heard
   with I2S1 DSP volume 32 and that PDN_I2S_CNT/I2S1_CNT are set (the shim
   writes them again from the ARM11).
2. Hardware: the speaker level. Amp step 19 (0 dB) is CSND unity; the
   CODEC's speaker gains are libn3ds' `hw_cal` defaults. The 3DS volume
   slider is not read.
3. Hardware: the headphone jack (re-log 57). Plug and unplug once with
   headphones and look for `[audio] headphones plugged in ... (CODEC headset
   detect XX)` / `unplugged`; they settle that GPIO2 bit 0 is the jack and 1
   means plugged (XX should differ between the two if the CODEC's own
   detection agrees). Then: sound only in the headphones while plugged, and
   the headphone level at a given iOS volume (`[audio] iOS headphone volume`).
   `[audio] jack: ...` warnings mean the fail-safe kept iOS on the speaker.
4. Microphone on hardware:
   - whether MIC_CNT's bits match GBATEK's DSi description (enable 15, empty 8, overrun 11, rate divider 2-3);
   - whether the CODEC's ADC needs more than libn3ds' microphoneInit plus page 0 registers 81/82;
   - how often the 1 ms service misses the 2.7 ms FIFO (`[audio] mic off` reports overruns). If it is often, a CDMA program could drain it instead.
   - The iOS input gain compensation assumes the same +23.65 dB for other categories (Voice Memos uses RecordAudio); check the level there.
5. iOS's headphone route is driven (re-log 57): route switch both ways, the headphone volume HUD, separate speaker/headphone volumes, `OldDeviceUnavailable` on unplug (what makes the iPod app pause). The headset remote/mic (`mikey`) is not.
