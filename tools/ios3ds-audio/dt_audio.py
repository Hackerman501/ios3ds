"""Device-tree side of ios3ds audio (imported by kernel/dtgen.py). See docs/audio.md.

Apple's own audio stack is kept: AppleARMPL080DMAC (dmac0), AppleS5L8900XI2SController
(i2s0), the I2C bus (i2c0) with the CS42L58 codec (audio0) and the LM48821 speaker amp
(spkr-amp). Their register blocks become fake pages; the shim emulates the PL080
(kernel/shim/audio.c) and answers the I2C transfers.

SPDX-License-Identifier: GPL-3.0-or-later
"""

# nodes kernel/dtgen.py used to delete, now kept
KEEP = ["arm-io/dmac0", "arm-io/i2c0", "arm-io/i2s0"]
# each gets its own fake register page
FAKEIO = ["dmac0", "i2c0", "i2s0"]
# i2c0 children whose drivers would run against an I2C bus that answers zeros:
# the PMU (battery, power button, shutdown decisions), accelerometer, tethered
DELETE = ["arm-io/i2c0/accelerometer", "arm-io/i2c0/pmu", "arm-io/i2c0/tethered"]

# Properties of /arm-io/i2c0/audio0 that point at deleted nodes. AppleARMFunction
# lookups do waitForService(AAPL,phandle == target) with no timeout
# (AppleARMPlatform 0x802e4698), so a dangling function-* would hang the codec
# driver's start forever.
#   function-hp_detect  -> pmu GPIO 2 (headphone detect). Without it
#                          AppleEmbeddedAudio skips headphone detection
#                          (0x806533a4..c0) and stays on the speaker route.
#   mikey               -> /arm-io/i2c1/mikey (headset remote), i2c1 is deleted
#   interrupts/-parent  -> pmu interrupt 0x16; only used for hp detect
AUDIO0_DROP = ["function-hp_detect", "mikey", "interrupts", "interrupt-parent"]

# The 3DS has a built-in microphone; the iPod has only the headset one
# ("ain2a-extmic"). AppleCS42L58Audio offers input data sources from these
# properties (0x80660294): 'line' always, 'imic' with "ain2b-intmic", 'emic'
# with "ain2a-extmic"; CoreMedia names them LineIn / MicrophoneBuiltIn /
# MicrophoneWired. Declaring the internal one gives iOS a mic that is always
# there, without a headset route.
AUDIO0_ADD = {"ain2b-intmic": b""}

DMAC0_IRQ = 0x10    # /arm-io/dmac0 'interrupts' (VIC0 source 16)

# Headphone detection, rebuilt on what the 3DS has (docs/audio.md, re-log):
# AppleEmbeddedAudio reads the jack through function-hp_detect (0x8065117c:
# non-zero = headphones) and re-reads it on audio0's interrupt 0
# (IOInterruptEventSource, 0x8065330c). The iPod wires both to its PMU; here
# the function is a pin of the (fake) SoC GPIO page that the shim sets from
# the ARM9's jack polling, and the interrupt a free VIC0 source the shim
# injects. Pin 0x0d00 = port 13 bit 0: data word at gpio page + 0x1a4
# (AppleS5L8900XGPIO 0x8066db60: port * 0x20 + 4); no DT function uses port 13.
# Flags 0x100: input, value not inverted (0x8066dcc4: raw ^ (flag byte 1 ^ 1)).
GPIO_PHANDLE = 0x0090c9a0   # /arm-io/gpio
VIC0_PHANDLE = 0x0090c3e0   # /arm-io/vic (VIC0)
HP_PIN = 0x0d00
HP_FLAGS = 0x00000100
HP_IRQ = 0x0c               # unassigned VIC0 source
AUDIO0_HP = {
    "function-hp_detect": b"".join(v.to_bytes(4, "little") for v in (GPIO_PHANDLE, 0x4750494f, HP_PIN, HP_FLAGS)),
    "interrupts": HP_IRQ.to_bytes(4, "little"),
    "interrupt-parent": VIC0_PHANDLE.to_bytes(4, "little"),
}


def apply(root):
    """Called by dtgen.generate() after the DELETE pass. A tree without the
    audio controllers (deleted, or a test template) is left alone."""
    if root.find("arm-io/i2c0") is None or root.find("arm-io/dmac0") is None:
        return
    codec = root.find("arm-io/i2c0/audio0")
    if codec is None:
        raise KeyError("dt_audio: no /arm-io/i2c0/audio0")
    for name in AUDIO0_DROP:
        codec.delete(name)
    for name, value in AUDIO0_ADD.items():
        codec.set(name, value)
    gpio, vic = root.find("arm-io/gpio"), root.find("arm-io/vic")
    if gpio is None or int.from_bytes(gpio.get("AAPL,phandle")[:4], "little") != GPIO_PHANDLE:
        raise ValueError("dt_audio: /arm-io/gpio is not phandle %#x" % GPIO_PHANDLE)
    if vic is None or int.from_bytes(vic.get("AAPL,phandle")[:4], "little") != VIC0_PHANDLE:
        raise ValueError("dt_audio: /arm-io/vic is not phandle %#x" % VIC0_PHANDLE)
    for name, value in AUDIO0_HP.items():
        codec.set(name, value)
    dmac = root.find("arm-io/dmac0")
    irq = dmac.get("interrupts")
    if irq is None or int.from_bytes(irq[:4], "little") != DMAC0_IRQ:
        raise ValueError("dt_audio: dmac0 interrupt is not %#x (shim injects VICVECTADDR16)" % DMAC0_IRQ)
