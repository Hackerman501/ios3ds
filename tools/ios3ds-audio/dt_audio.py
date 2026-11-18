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
    dmac = root.find("arm-io/dmac0")
    irq = dmac.get("interrupts")
    if irq is None or int.from_bytes(irq[:4], "little") != DMAC0_IRQ:
        raise ValueError("dt_audio: dmac0 interrupt is not %#x (shim injects VICVECTADDR16)" % DMAC0_IRQ)
