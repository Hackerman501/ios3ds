# Wi-Fi: networking for iOS on the Old 3DS

Status: **code complete, never run on hardware or in QEMU.** The ARM9 driver,
WPA2 supplicant, frame mailbox, shim sysctl and iOS daemon all build; the
pure parts (mailbox, framing, config, crypto, a full 4-way handshake,
firmware extraction) pass host tests (`make -C tools/ios3ds-wifi test`, also
run by `make test`). The emulator has no Wi-Fi device yet (section 7).

```
 AR6014 (SDIO)        ARM9 monitor                     XNU shim              iOS
 -------------        ------------                     --------              ---
 TMIO ctrl 2   <--->  boot/arm9/wifi/ar6k.c   netmbox_t   kern.ios3ds_net    ios3ds-wifid
 0x10122000           BMI fw upload, HTC/WMI  @0x208A0000 (RW opaque sysctl,  <-> IOUserEthernet "en0"
 (polled)             WPA2 4-way (wpa2.c)     16+16 slots  tools/ios3ds-wifi/      <-> configd DHCP
                      802.3 <-> WMI framing   x 1536 B     shim/net.c)            (ipconfig set en0 DHCP)
```

## 1. Verified facts

| Fact | Source |
|---|---|
| Wi-Fi SDIO controller at `0x10122000` (TMIO "controller 2") is mapped for **both** ARM11 and ARM9 | 3dbrew "IO Registers" (`A11/A9`, "WIFI SDIO bus registers", used by NWM) |
| The ARM9 interrupt controller has no line for it (only `SDIO_1`, `SDIO_3`): the ARM9 must poll | 3dbrew "IRQ Registers"; libn3ds `tmio.h` (ARM9 sees controllers 1/3, ARM11 sees 2/3) |
| `CONFIG11` (0x10140000) and `GPIO` (0x10147000) are mapped for the ARM9 too | 3dbrew "IO Registers" |
| Power-up: `CFG11_WIFICNT` (0x10140180) = 1, GPIO 2_2 (`0x10147014` bit 0) = 0 selects the SDIO chip, GPIO 3_12 (`0x10147028` bit 0) = 1 releases Atheros nRESET, then ~2 ms | libn3ds `arm11/drivers/wifi/wifi.c`, `gpio.h` |
| AR6014: chip id `0D000001h` at `[0040ECh]`, BMI ROM version `2300006Fh`, host interest area `00520000h`, BMI_DONE entry `927000h` | GBATEK "DSi Atheros Wifi" chapters; dsiwifi `wifi_card.twl.c` |
| BMI register setup before upload (`hi_app_host_interest`=2, scratch0 \|= 8, `WLAN_SYSTEM_SLEEP` \|= 1, clock control = 5, `[4020h]`=0, `hi_mbox_io_block_sz`=80h, `hi+74h`=63h) | dsiwifi `wifi_card_wlan_init` |
| MBOX packet headers, data frame layout (`00 1C`/rssi + dst + src + BE length + LLC/SNAP), trailer/lookahead | GBATEK "MBOX Transfer Headers"; dsiwifi `wifi_card_mbox0_readpkt`, `data_send_*` |
| HTC service connect list (5 services) + SETUP_COMPLETE, `INT_STATUS_ENABLE` = `010300D1h` | dsiwifi `htc_handle_pkt` |
| WMI_CONNECT layout and DSi/3DS enum values (auth 5 = WPA2-PSK, crypto 1/3/4 = none/TKIP/AES); TKIP keys need the MIC halves swapped | GBATEK "WMI Connect Functions"; dsiwifi `wmi_connect_cmd`, `wmi_add_cipher_key` |
| Firmware lives in NWM's .code as six blocks; Type1 main of newer NWM may hang, Type4 works for normal internet with heartbeat timeout 0 and event 1025h ignored | GBATEK "3DS Files - Module NWM" |
| `IOUserEthernet.kext` is prelinked (0x80512000), publishes `IOUserEthernetResource`, requires entitlement `com.apple.networking.ethernet.user-access` | kernelcache: `scripts/kexts.py`, strings |
| IOKit.framework exports `IOEthernetControllerCreate/ReadPacket/WritePacket/SetLinkStatus/GetIONetworkInterfaceObject`, `kIOEthernetHardwareAddress`; `ReadPacket` = `recv(CFSocketGetNative(*(ref+0x28)), buf, len, 0)` | dyld cache (8C148), disassembly |
| `/usr/sbin/ipconfig`, `scutil`, configd with `IPConfiguration`/`IPMonitor` bundles and `mDNSResponder` are in the stock root fs and not disabled by `rootfs/recipe.json` | `build/tools/hfsplus ... ls` |

Not verified (needs a console): that ARM9 writes to `CFG11_WIFICNT`/GPIO
really take effect, SDIO timing at HCLK/4 from the ARM9, that WMI_CONNECT
with our scan result associates with every AP, and the IOUserEthernet path
end-to-end.

## 2. ARM9 side (`boot/arm9/wifi/`)

| File | Role |
|---|---|
| `sdio.c` | Polled TMIO host: CMD5/3/7 enumeration, 4-bit bus, block size 128, CMD52, CMD53 block mode (FIFO32). Register usage and command encoding follow dsiwifi `wifi_sdio.twl.c`; every wait is bounded. |
| `ar6k.c` | State machine, one bounded step per monitor-loop pass: POWER -> BMI -> FWLOAD (one 256-byte chunk per pass) -> BOOT (`hi_board_data_initialized`) -> HTC -> WMI -> SCAN -> AUTH -> UP. |
| `wpa2.c` | SHA-1, HMAC, PBKDF2, 802.11i PRF, AES-128, RFC 3394 unwrap, EAPOL-Key 4-way and group handshake (CCMP pairwise; CCMP or TKIP group). Written from the standards. |
| `wifi_frame.c` | Ethernet II <-> WMI data frames (LLC/SNAP RFC 1042 and bridge-tunnel). |
| `wifi_ring.c` | ARM9 half of the mailbox. |
| `wifi_cfg.c` | `sd:/ios3ds/wifi.cfg` parser. |
| `wifi.c` | `wifi_init()` (before the XNU jump: config, PBKDF2, mailbox init) and `wifi_poll()` (each monitor loop pass: driver step, up to 4 frames each way). Loopback backend. |
| `vnic.c` | Backend for the proposed QEMU device (section 7); only used with `backend=qemu`. |

Connect sequence after WMI_READY: error-report bitmask 7Fh, heartbeat timeout
0, BSS filter "all", probed SSID (hidden networks), START_SCAN. BSSINFO events
for our SSID give channel, BSSID and the RSN group cipher; on SCAN_COMPLETE
(or ~4000 polls) we send power mode 2, disconnect timeout 5, keepalive 0 and
WMI_CONNECT. If the SSID was not seen, channel and BSSID stay 0 and the
firmware searches by itself (unverified on this firmware). After
WMI_CONNECT_EVENT the supplicant answers EAPOL-Key frames (sent as
`00 1C`/SNAP data frames, mailbox control word 0), installs PTK and GTK with
WMI_ADD_CIPHER_KEY and sends WMI_SYNCHRONIZE; then frames flow. A disconnect
event triggers a reconnect.

The nonce source is the ARM9 PRNG register, a timer and SDIO status hashed
with SHA-1 (no TRNG on the ARM9).

### wifi.cfg

```
ssid=My Network          # 1-32 bytes, taken verbatim
psk=correct horse        # 8-63 chars, or 64 hex digits (= PMK, skips PBKDF2)
security=wpa2            # or open (default: wpa2 if psk given)
backend=auto             # auto|ar6014|qemu|loop|off
fw=0:/ios3ds/wifi-fw.bin
verbose=0                # 1 = state changes, 2 = every frame
mac=02:..                # qemu/loop backends only
```

No file means networking off; the mailbox then reports backend NONE.

## 3. Firmware (not shipped)

The AR6014 ROM cannot run Wi-Fi by itself; Nintendo's Xtensa firmware must be
uploaded after every power-up. ios3ds never contains it. The user extracts it
from **their own console**:

1. GodMode9: `[1:] SYSNAND CTRNAND/title/00040130/00002d02/content/<id>.app`
   (NWM, the Wi-Fi sysmodule) -> "NCCH image options..." -> "Extract .code"
   (menu wording may differ between GodMode9 versions).
2. On a PC: `tools/ios3ds-wifi/nwmfw.py <tid>.code -o wifi-fw.bin`
   (accepts decompressed or BLZ-compressed .code), copy to `sd:/ios3ds/`.

`nwmfw.py` finds the literal pool GBATEK describes (constants `524C00h`,
`53FE18h`, `527000h`; "newer" and "older" layouts), checks the fixed sizes
(stub data 38h, stub code 316h, database 1E8h) and writes a container:
16-byte header (`'I3WF'`, version 1, part count, chip id) + 8 x
`{kind, dst, off, len}` + blocks. Upload order in `ar6k.c` (GBATEK "DSi SD/MMC
Firmware Wifi Firmware"): stub data -> 524C00h, stub code -> 527000h,
BMI_EXECUTE(527000h + 400000h, 527000h), main via BMI_LZ_STREAM_START/DATA ->
524C00h, database -> 53FE18h, `hi_dset_list_head` = 53FE18h, BMI_DONE.
Default main variant is Type4 (`--main type1` to try the other).
(dsiwifi's disabled upload code re-sends the stub data after the main
firmware; GBATEK's order does not, and neither do we. Try that first if the
firmware never reports `hi_board_data_initialized`.)

## 4. Mailbox and kernel side

`netmbox_t` (`boot/common/wifi_proto.h`) at PA `0x208A0000` / VA `0x808A0000`:
FCRAM between the XNU log ring (ends `0x20890000`) and `topOfKernelData`
(`0x20900000`), same 1 MiB section as the shim, so XNU maps it and never hands
it to the VM. 128-byte header with one writer per cache line (the
`blkmbox_t` rule: the ARM9 sees FCRAM uncached, the ARM11 cleans/invalidates
lines), then 16 RX and 16 TX slots of 1536 bytes. Indices are free-running;
producer writes the slot, then the index. A tail the ARM9 does not believe
(ARM9 restarted under iOS) makes it drop until the ARM11's next read resyncs.

`kern.ios3ds_net` (`tools/ios3ds-wifi/shim/net.c`, built into shim.bin,
registered next to `kern.ios3ds_input` on the first root-disk I/O): one 4 KiB
read/write opaque OID (`sysctl_register_oid` 0x80159c54,
`sysctl_handle_opaque` 0x80159242, no kernel patch, so there is no
`kernel/patches-wifi.json`). Read = status (link, MAC, backend, state, TX
free) + as many pending RX frames as fit, *not consumed*. Write
(`NETIO_CMD`) = absolute ack sequence number + TX frames. Because the handler
builds the outgoing batch before it sees the written data, the daemon writes
first (oldp = NULL) and then reads. Writes need root (the daemon runs as
root). The shared 4 KiB buffer is guarded by a busy flag (`EBUSY` for a
concurrent caller).

## 5. iOS side

`ios3ds-wifid` (`tools/ios3ds-wifi/daemon`, launchd job
`com.ios3ds.wifi`, signed with `com.apple.networking.ethernet.user-access`):
waits until the mailbox has a backend and MAC, creates an
`IOEthernetController` with `kIOEthernetHardwareAddress` = the AR6014's MAC,
makes its socket non-blocking and loops: drain iOS's TX queue into one
write, read the next RX batch, `WritePacket` each frame, mirror the link
state with `SetLinkStatus`, `poll()` the socket for at most 4 ms when idle.
On the first link-up it runs the stock `/usr/sbin/ipconfig set <if> DHCP`, so
configd's IPConfiguration agent does DHCP, routes and DNS (IPMonitor,
mDNSResponder). `nodhcp` leaves addressing to SystemConfiguration
preferences instead. The interface should be `en0` (the only Ethernet-class
interface: AppleBCMWLAN never matches without Apple's SDIO controller).

Build: `make -C tools/ios3ds-wifi/daemon` (needs `scripts/ios-cc`), then the
usual rootfs build picks up `build/rootfs-add/usr/libexec/ios3ds-wifid` and the
plist via `rootfs/recipe.json`.

## 6. Bring-up checklist on a console

The 3DS console and `sd:/ios3ds/log.txt` show each state change:
1. `SDIO up, chip id 0D000001 (AR6014)`: power, GPIOs and TMIO work from the
   ARM9. If CMD5 times out: check `CFG9_SDMMCCTL` bit 2 (port power), try a
   longer reset delay, compare with libn3ds `SDIO_init` (CMD0 first, CMD5 with
   OCR 3.2-3.3 V).
2. `BMI ROM version 2300006F`.
3. `fw part ...` x4, `stub executed`, `firmware running`.
4. `HTC services connected`, `WMI ready, MAC ...`.
5. `connect "ssid" ... seen in scan`, `associated`, `EAPOL event 1`,
   `EAPOL event 2`, `link up`.
6. On iOS: `[wifid] interface en0 up`, `link up`, `ipconfig set en0 DHCP`.

`backend=loop` exercises everything above the driver on hardware without a
network (frames from iOS come straight back).

## 7. QEMU: device needed

QEMU's `nintendo3ds` board has no Wi-Fi. Two options, cheapest first:

**A. Paravirtual NIC "ctr-vnic"** (ARM9 side already written: `vnic.c`,
`backend=qemu`). Not real hardware; tests the mailbox, shim, daemon, iOS
networking and DHCP against QEMU's user-mode network (slirp gives DHCP, DNS,
NAT). Map a 4 KiB MMIO region at `0x101F0000` (unused on the 3DS), backed by a
QEMU `NICState` (`-nic user,model=ctr-vnic`):

| Offset | Access | Meaning |
|---|---|---|
| 000h | R | ID `'I3NC'` (`434E3349h`) |
| 004h | RW | bit 0 RX enable, bit 1 reset (self-clearing) |
| 008h | R | bit 0 link up, bit 1 RX frame waiting, bit 2 TX idle |
| 00Ch/010h | R | MAC bytes 0-3 / 4-5 (byte 0 in bits 0-7) |
| 014h | R | length of the frame in the RX buffer, 0 = none |
| 018h | W | write 1: drop that frame, load the next (queue >= 16 frames, drop beyond) |
| 01Ch | W | write n: send TX buffer bytes 0..n-1 as one frame |
| 400h-9FFh | R | RX buffer |
| A00h-FFFh | RW | TX buffer |

No interrupt is needed (the ARM9 polls). About 150 lines of QEMU C
(`can_receive`/`receive` into a small queue, `qemu_send_packet` on TX_LEN).

**B. TMIO SDIO + AR6K model** (to test `sdio.c`/`ar6k.c`): a TMIO register
model at `0x10122000` that implements CMD5/3/7/52/53, plus an AR6K function-1
model: CCCR, `COUNT4`/lookahead registers, BMI command parser (accept and
discard uploads, answer GET_TARGET_ID with `2300006Fh`), host interest word
58h = 1 after BMI_DONE, then canned HTC_READY / WMI_READY / BSSINFO /
SCAN_COMPLETE / CONNECT events and a built-in WPA2 authenticator (or open
network) that bridges data frames to a QEMU NIC. Much bigger; only worth it
if hardware debugging stalls.

## 8. Shared-file hooks

- `boot/Makefile`: wifi sources added to `ARM9_SRC`.
- `boot/arm9/main.c`: `#include "wifi/wifi.h"`, `wifi_init()` before the jump,
  `wifi_poll()` in the monitor loop.
- `scripts/mkboot.py`: `tools/ios3ds-wifi/shim/net.c` added to the shim build.
- `kernel/shim/blk.c`: `shim_net_register()` next to `shim_input_register()`.
- `rootfs/recipe.json`: adds `ios3ds-wifid` and `com.ios3ds.wifi.plist`.
- `Makefile`: `make test` also runs `make -C tools/ios3ds-wifi test`.

## 9. Open items

- Hardware bring-up (section 6); MCU Wi-Fi LED (I2C, optional).
- Data frames are sent with mailbox control word 0; dsiwifi uses `2008h` for
  IP traffic. Try that if unicast data does not flow after the handshake.
- TX flow control: no HTC credit accounting from the receive trailer yet;
  heavy traffic may overflow the firmware's MBOX ("Mbox Full" in dsiwifi).
- WPA (TKIP pairwise), WEP and enterprise networks are not supported.
- Faster path later: wake the daemon from the shim instead of 4 ms polling,
  CMD53 byte mode for BMI instead of one CMD52 per byte.
