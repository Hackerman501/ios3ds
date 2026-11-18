# Wi-Fi: networking for iOS on the Old 3DS

Status: **works end to end in QEMU, never run on hardware.** In the
`nintendo3ds` machine the unchanged ARM9 driver brings up an emulated AR6014
over SDIO, associates with an emulated WPA2 access point, and iOS gets a DHCP
lease, a default route and DNS, and fetches a page from the host (section 7,
`emu/wifi_test.py`). The model is written from ath6kl, dsiwifi, GBATEK and
IEEE 802.11i, not from our driver; what it cannot prove, and the hardware
assumptions that are still open, are in sections 6 and 7.2. Host tests:
`make -C tools/ios3ds-wifi test` (also run by `make test`).

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
| `sdio.c` | Polled TMIO host: CMD5/3/7 enumeration, 4-bit bus, block size 128, CMD52, CMD53 block mode and byte mode (FIFO32). Register usage and command encoding follow dsiwifi `wifi_sdio.twl.c`; every wait is bounded. |
| `ar6k.c` | State machine, one bounded step per monitor-loop pass: POWER -> BMI -> FWLOAD (one 256-byte chunk per pass) -> BOOT (`hi_board_data_initialized`, 3 s) -> HTC (HTC_READY, 5 s) -> WMI (WMI_READY, 5 s) -> SCAN (10 s) -> AUTH -> UP. Timeouts use the monitor's clock (timers 2+3). After "SDIO up" a FIFO self-test (CMD52 pattern into the function-1 scratch registers, read back with one CMD53) decides between CMD53 block transfers and a CMD52-per-byte fallback for the mailbox. The WMI and data endpoints are the ones the firmware returns in its CONNECT_SERVICE responses. |
| `wpa2.c` | SHA-1, HMAC, PBKDF2, 802.11i PRF, AES-128, RFC 3394 unwrap, EAPOL-Key 4-way and group handshake (CCMP pairwise; CCMP or TKIP group). Written from the standards. |
| `wifi_frame.c` | Ethernet II <-> WMI data frames (LLC/SNAP RFC 1042 and bridge-tunnel). |
| `wifi_ring.c` | ARM9 half of the mailbox. |
| `wifi_cfg.c` | `sd:/ios3ds/wifi.cfg` parser. |
| `wifi.c` | `wifi_init()` (before the XNU jump: config, PBKDF2, mailbox init) and `wifi_poll()` (each monitor loop pass: driver step, up to 4 frames each way). Loopback backend. |
| `vnic.c` | Backend for a paravirtual QEMU NIC that was never built (section 7); `backend=qemu` fails cleanly. |

Connect sequence after WMI_READY: error-report bitmask 7Fh, heartbeat timeout
0, BSS filter "all", probed SSID (hidden networks), START_SCAN. BSSINFO events
for our SSID give channel, BSSID and the RSN group cipher; on SCAN_COMPLETE
(or after 10 s) we send power mode 2, disconnect timeout 5, keepalive 0 and
WMI_CONNECT. If the SSID was not seen, channel and BSSID stay 0 and the
firmware searches by itself (unverified on this firmware). After
WMI_CONNECT_EVENT the supplicant answers EAPOL-Key frames (sent as
`00 1C`/SNAP data frames, mailbox control word 0), installs PTK and GTK with
WMI_ADD_CIPHER_KEY and sends WMI_SYNCHRONIZE; then frames flow. A disconnect
event (logged with its WMI reason and 802.11 code) brings the link down and
reconnects after 5 s; the third failed handshake in a row logs "is psk= in
wifi.cfg right?".

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
nettest=http://10.0.2.2:8080/ example.com   # optional iOS self-test (section 5)
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

**Missing or broken firmware fails safe.** Without the file the ARM9 logs
once `[wifi] 0:/ios3ds/wifi-fw.bin: not found (...). Create it with
tools/ios3ds-wifi/nwmfw.py` and `backend ar6014 not available: networking
off`, never powers the chip, and iOS sees "no backend" (ios3ds-wifid waits
quietly). A file that is not an `I3WF` container logs `bad container` the
same way. Firmware that uploads but does not start ends in state `failed`
with one line naming the step (`no HTC_READY`, ...). QEMU checks the first
case (`emu/wifi_test.py --case nofw`). The test's `wifi-fw.bin` is random
bytes in the container layout; no Nintendo or Atheros code is in the
repository or the test.

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
It finds the interface's BSD name by its MAC in the link-level address list
(`getifaddrs`, `AF_LINK`): IONetworkStack names the interface asynchronously,
so the registry's "BSD Name" is often not there yet. In QEMU it is `en0` (the
only Ethernet-class interface: AppleBCMWLAN never matches without Apple's SDIO
controller).

**Addressing:** configd configures a new Ethernet interface by itself: its
default network set has DHCP, so IPConfiguration leases an address,
IPMonitor installs the default route and hands the DHCP DNS servers to
mDNSResponder (seen in QEMU, section 7.3). Only if the interface has no IPv4
address 15 s after link-up does the daemon run the stock
`/usr/sbin/ipconfig set <if> DHCP`; `nodhcp` never does.

**Self-test:** `nettest=<url> [dns name]` in wifi.cfg reaches iOS through
the mailbox (`netmbox_t.nettest`, `netio_t.nettest`, protocol version 2).
Once the interface has a name the daemon starts `/usr/libexec/ios3ds-nettest
<if> <url> [name]` (`tools/ios3ds-wifi/daemon/nettest.c`), which logs one
`[nettest]` line per step: the DHCP address, the IPv4 default route (routing
table via `sysctl NET_RT_DUMP`), `getaddrinfo(name)` (mDNSResponder with the
DHCP DNS server) and an HTTP/1.0 GET of the URL over BSD sockets, then
`all steps passed` or `FAILED`. Without `nettest=` nothing extra runs.

Build: `make -C tools/ios3ds-wifi/daemon` (needs `scripts/ios-cc`), then the
usual rootfs build picks up `build/rootfs-add/usr/libexec/ios3ds-wifid` and the
plist via `rootfs/recipe.json`.

## 6. Bring-up checklist on a console

Put `wifi.cfg` (with `verbose=1` and, for the first runs,
`nettest=http://<a PC on the LAN>:8080/ <some name>`) and `wifi-fw.bin` on
the card. The 3DS console and `sd:/ios3ds/log.txt` show each step; QEMU
prints the same lines (section 7.3). Each step settles one unverified
assumption:

| Log line | Settles | If it fails |
|---|---|---|
| `[wifi] SDIO up, chip id 0d000001 (AR6014)` | the ARM9 can power the chip (`CFG11_WIFICNT`, GPIO 2_2/3_12) and drive TMIO controller 2 at 0x10122000 by polling | CMD5 timeout: check `CFG9_SDMMCCTL` bit 2 (port power), longer reset delay, libn3ds `SDIO_init` (CMD0 first, CMD5 with OCR 3.2-3.3 V) |
| `[wifi] TMIO2 32-bit FIFO works` | the ARM9 can use the FIFO at 0x1012210C (dsiwifi's 3DS ARM11 build uses a mirror at +0x200000 instead) | `FIFO check failed ... falls back to CMD52`: Wi-Fi still works, slowly; try the +0x200000 mirror next |
| `[wifi] BMI ROM version 2300006f` | BMI over CMD52, COUNT4 credits | |
| `[wifi] fw part 1..4`, `stub executed`, `firmware running (board data initialised)` | upload order, the stub setting `hi_board_data_initialized` (model assumption) | `firmware start (hi_board_data_initialized never set)`: re-send the stub data after main (dsiwifi's order), section 3 |
| `[wifi] HTC ready: N credits of M bytes`, `HTC services connected: 0 (WMI endpoint 1, data 2/3/4/5)` | the Type4 firmware boots and answers HTC | `no HTC_READY`: try `--main type1`; compare BMI writes with dsiwifi |
| `[wifi] WMI ready, MAC ...` | WMI with the 2-byte header | |
| `[wifi] connect "..." ..., seen in scan` | BSS filter 1 + START_SCAN over all channels reports BSSINFO (dsiwifi scans per channel) | `not seen in scan`: the firmware searches by itself; if that fails, scan per channel like dsiwifi (`SET_CHANNEL_PARAMS` + `START_SCAN`) |
| `[wifi] associated, channel ... MHz`, `EAPOL event 1`, `EAPOL event 2`, `link up` | our RSN IE in message 2 matches the firmware's association request (we send capabilities 0) | AP deauths after message 2 (`disconnected (WMI reason 4, 802.11 code 17)`): the firmware's IE differs, copy it from the CONNECT event's assoc request |
| `[wifid] interface is en0`, `[wifid] en0 configured by configd` | IOUserEthernet + configd's automatic DHCP on hardware | `ipconfig set en0 DHCP: status ...` runs after 15 s |
| `[nettest] en0 address ...`, `default route via ...`, `dns ... -> ...`, `http ... -> HTTP/1.0 200 OK`, `all steps passed` | data in both directions through the firmware with our keys | rx/tx only stall: try mailbox control word `2008h` for data (dsiwifi), section 9 |

Not modelled and still to try on a console if WMI commands go unanswered:
dsiwifi writes 3 to the word that `hi_app_host_interest` points at after
`WMI_REG_DOMAIN_EVENT` ("allows more commands to be sent"); we do not.

`backend=loop` exercises everything above the driver on hardware without a
network (frames from iOS come straight back).

## 7. QEMU: the Wi-Fi model (`emu/qemu/hw/arm/ctr_wifi.c`)

The `nintendo3ds` board has the Wi-Fi slot modelled at the bus level, so the
real driver (`sdio.c`, `ar6k.c`, `wpa2.c`) runs unchanged against it
(`backend=auto`, nothing QEMU-specific). Everything the model does is written
from the references below, not from our driver; where they disagree or say
nothing, the model's choice is listed under "assumptions".

```
 ARM9 driver --MMIO--> TMIO controller 2 @0x10122000 (polled, no IRQ)
   --CMD5/3/7/52/53--> SDIO card: CCCR, FBR, CIS (Atheros 0271/0201), function 1
     --mailbox 0, function-1 registers--> AR6014 "target"
         BMI ROM: command parser, RAM bookkeeping (firmware contents never run)
         firmware: HTC (services, credits), WMI (scan, connect, keys, events)
     --"air"--> access point: beacons/probe responses, association,
                WPA2-PSK authenticator (IEEE 802.11i), CCMP keys checked
     --Ethernet II--> QEMU NIC backend: -nic user,model=ctr-wifi (slirp:
                DHCP 10.0.2.15, router + host 10.0.2.2, DNS 10.0.2.3, NAT)
```

Run it: `python3 emu/wifi_test.py` (section 7.3). Manually: boot with
`-nic user,model=ctr-wifi` (plus `-global ctr-wifi.ssid=...`,
`-global ctr-wifi.psk=...`) and put a `wifi.cfg` with the same SSID/PSK and a
`wifi-fw.bin` on the SD image.

### 7.1 What is modelled, and from which source

| Layer | Model | Source |
|---|---|---|
| Power | card answers only with `CFG11_WIFICNT` bit 0 = 1, GPIO 3_12 (nRESET) = 1, GPIO 2_2 = 0; nRESET low = cold reset (RAM, SDIO state) | libn3ds `WIFI_init`, `gpio.h` |
| TMIO | command/response, STAT0/STAT1 write-0-to-clear, 32-bit FIFO (`0x10C`) with `DATA32CTL` RX-full / STAT1 TXRQ per block, BLKLEN32/BLKCNT, CMD timeout when nothing answers, data timeout when the controller's block count x length differs from the CMD53 byte count, CRC error when card and controller bus widths differ | the board's TMIO1 model, libn3ds `tmio.h`, dsiwifi `wifi_sdio.twl.c` |
| SDIO | CMD5 (R4: inquiry with OCR 0 is "not ready", ready after an OCR with a common voltage), CMD3 (RCA 1), CMD7 select, CMD52 R/W/RAW, CMD53 byte and block mode with incrementing or fixed address; function 1 only after `CCCR 0x02` bit 1; `0x07` bus width; block sizes at `0x10`/`0x110` | SDIO Simplified Spec 3.00 |
| Function 1 | `HOST_INT_STATUS` (bit 0 = mailbox 0 has data, bit 7 = error), `ERROR_INT_STATUS` (TX overflow / RX underflow, write-1-clear), `RX_LOOKAHEAD_VALID` (bit 0 = 4 bytes ready), `RX_LOOKAHEAD0`, `COUNT[4]`/`COUNT_DEC[4]` = BMI credit (reading the decrementing copy takes it, each finished command returns it), the diagnostic window (data `0x474`, write address `0x478`, read address `0x47C`; writing the address LSB starts the cycle) | ath6kl `target.h`, `sdio.c` (`ath6kl_sdio_bmi_credits`, `ath6kl_set_addrwin_reg`), dsiwifi |
| Mailbox 0 | one byte stream each way. Windows `0x000-0x0FF`, `0x800-0xFFF`, `0x1000-0x3FFF`; a host write that touches the window's last address ends a message (HTC mode). Target messages are padded to `hi_mbox_io_block_sz` | ath6kl `hif.h`, dsiwifi (`0x4000 - len`, byte register `0xFF`) |
| BMI | `NOP, DONE, READ/WRITE_MEMORY, EXECUTE, SET_APP_START, READ/WRITE_SOC_REGISTER, GET_TARGET_ID (2300006Fh), LZ_STREAM_START, LZ_DATA`; data chunks up to 200h bytes; RAM 0x500000-0x53FFFF incl. the host interest area at 0x520000; `0x40EC` = chip id 0D000001h; `RESET_CONTROL` bit 8 = warm reset back into the ROM (`RESET_CAUSE` 2). Wedges (log line, no reply) on unknown commands, oversized chunks, writes outside RAM, `EXECUTE` of an address nothing was uploaded to, `LZ_DATA` without `LZ_STREAM_START` | dsiwifi `ath/bmi.h`, `wifi_card.twl.c`; GBATEK; ath6kl `bmi.c` |
| Firmware start | `BMI_DONE` + 50 ms: runs only if main firmware was streamed (`LZ_DATA` >= 1 KiB), the stub was executed and `hi_dset_list_head` points at uploaded data; otherwise the target stays dead | GBATEK upload order |
| HTC | `HTC_READY` (8 credits of 1664 bytes), `CONNECT_SERVICE` for services 0100h-0104h (endpoints handed out in order: 1 = WMI, 2-5 = data; unknown/duplicate services refused), `SETUP_COMPLETE(_EX)`; credits come back in RX trailers (`HTC_FLG_RX_TRAILER`, record 1), and a message with `NEED_CREDIT_UPDATE` gets an empty EP0 credit report if nothing else is sent | ath6kl `htc.h`, `htc_mbox.c`; dsiwifi `htc.twl.c` |
| WMI | 2-byte command header (AR6002-era firmware). Commands with a wrong parameter size get `WMI_CMD_ERROR_EVENT` (INVALID_PARAM). Events: `READY` (MAC, phy cap), `REG_DOMAIN`, `BSSINFO` (beacon or probe response with SSID, rates, DS, RSN IEs; SNR 40), `SCAN_COMPLETE`, `CONNECT` (channel, BSSID, intervals, assoc request/response IEs), `DISCONNECT` (802.11 code, BSSID, WMI reason), `GET_CHANNEL_LIST` reply. Scan: 30 ms per channel, honours the channel list, BSS filter (none / all / probed-SSID) and a hidden AP (only found by a directed probe) | dsiwifi `ath/wmi.h`, `wmi.twl.c`; ath6kl `wmi.h`; GBATEK "WMI" |
| Connect | `WMI_CONNECT` (52 bytes) must name the AP's SSID; channel/BSSID must match if given; WPA2 AP needs auth 5, pairwise 4 (AES), group = the AP's group cipher; open AP needs 1/1. Mismatch = `DISCONNECT` with the 802.11 status a real AP would use (41/42/43) | GBATEK "WMI Connect", 802.11-2007 7.3.1.9 |
| Authenticator | PMK = PBKDF2(psk, ssid) (GLib HMAC-SHA1), message 1 (ANonce), message 2 checked (replay counter, MIC with the PTK from PRF-384, RSN IE equal to the association request's, compared like hostapd), message 3 (AP RSN IE + GTK KDE, AES key wrap with QEMU's AES), message 4 MIC; retransmit every 1 s, deauth after 4 tries (reason 15). Written from IEEE 802.11i, not from `wpa2.c` | IEEE 802.11i-2004 8.5, RFC 3394, hostapd behaviour |
| Keys | `WMI_ADD_CIPHER_KEY` (45 or 51 bytes): pairwise must equal the AP's TK (type AES), group must equal the GTK with the AP's key index (TKIP: MIC halves swapped, as dsiwifi). Unicast frames are delivered/accepted only with the right TK, group frames only with the right GTK, nothing but EAPOL before message 4 | ath6kl `wmi.h` (`key_usage`), dsiwifi `wmi_add_cipher_key` |
| Data | host TX on a data endpoint: 2-byte WMI data header (`info & 3` = 0), 802.3 header, LLC/SNAP; source must be the station MAC. RX: `rssi, info, 802.3, LLC/SNAP` on the BE endpoint. Up to 24 KiB queued towards the host, beyond that frames are dropped like a full firmware buffer | dsiwifi `data_handle_pkt`, GBATEK "MBOX Transfer Headers" |

Properties (`-global ctr-wifi.<name>=`): `ssid` (default `ios3ds-qemu`),
`psk` (`ios3ds-test-passphrase`), `security` (`wpa2`/`open`), `group`
(`ccmp`/`tkip`), `channel` (1-13, default 6), `hidden`, `debug` (0-3; 1 logs
every milestone as `ctr-wifi: <virtual seconds> ...`), `mac` (the station MAC
the firmware reports; QEMU's NIC default 52:54:00:12:34:56).

### 7.2 Fidelity limits (things the model cannot prove)

- **No firmware is executed.** Upload contents are ignored; only sizes,
  addresses and order are checked. Whether Nintendo's Type4 main firmware
  behaves like the model's WMI layer is open (section 6).
- **No radio, no CCMP.** Encryption is modelled as "keys equal" checks. No
  rekeying (group message 1/2), no PMKSA caching, no power save, no roaming,
  no beacon loss.
- **No timing.** TMIO commands finish immediately, CMD5 is ready at the
  second try, the firmware boots in 50 ms. Real SDIO clocks, busy states and
  FIFO stalls are not modelled.
- **No interrupts.** Neither the TMIO2 IRQ (GIC 64/65 on the ARM11) nor the
  SDIO card interrupt; the ARM9 polls anyway.
- **No HTC back-pressure.** The model processes each host message at once,
  so a host that ignores credits never overflows it (real firmware: "Mbox
  Full", see dsiwifi).
- **Assumptions** (no source; settle on hardware with the log lines in
  section 6): the board-data stub sets `hi_board_data_initialized` = 1
  (dsiwifi only shows it being 1 after boot); the firmware's association
  RSN IE has capabilities 0 (our message 2 sends 0; a different value would
  make a strict AP reject message 2); BSS filter 1 reports BSSINFO during a
  START_SCAN over all channels (dsiwifi scans one channel at a time with
  filter 1); `REG_DOMAIN` follows `READY`; the AP sends 802.1X version 2.

### 7.3 `emu/wifi_test.py`

Writes `wifi.cfg` (with `nettest=`, section 5) and a dummy `wifi-fw.bin`
(random bytes in the `nwmfw.py` container: the model never runs firmware) to
`build/sd.img`, starts an HTTP server on the host's `127.0.0.1:8080` (iOS
reaches it as `10.0.2.2:8080` through slirp), boots, checks every step in
the log and removes the two files again. Cases: `--case ok` (WPA2, default),
`open`, `nocfg` (Wi-Fi must stay off and the card unpowered), `nofw`
(firmware missing: one log line, Wi-Fi off), `wrongpsk` (message 2 MIC
rejected, AP deauths after 4 tries, no link), psk hint after the third try), `nofifo` (model property
`fifo32=off`: the driver's FIFO self-test fails and the mailbox runs over
CMD52), `hidden` (AP only answers a directed probe), `tkip` (TKIP group key,
MIC halves swapped). `--icount 2` gives hardware-like timing.

Results (2026-11-18, re-log 45): `ok` PASS in plain QEMU and with
`--icount 2` (SpringBoard on screen, `[nettest] all steps passed`, the host's
server logged `GET /ios3ds-nettest`); `open`, `nocfg` (full boot to
SpringBoard), `nofw`, `wrongpsk`, `nofifo`, `hidden`, `tkip` PASS. The
status bar shows "Searching..." and no Wi-Fi bars (section 9).

The paravirtual "ctr-vnic" NIC that an earlier version of this section
proposed (`vnic.c`, `backend=qemu`, register spec in `boot/common/wifi_proto.h`)
bypasses the driver and was not built; `backend=qemu` fails cleanly on this
board ("backend qemu-vnic not available").

## 8. Shared-file hooks

- `boot/Makefile`: wifi sources added to `ARM9_SRC`.
- `boot/arm9/main.c`: `#include "wifi/wifi.h"`, `wifi_init()` before the jump,
  `wifi_poll()` in the monitor loop.
- `scripts/mkboot.py`: `tools/ios3ds-wifi/shim/net.c` added to the shim build.
- `kernel/shim/blk.c`: `shim_net_register()` next to `shim_input_register()`.
- `rootfs/recipe.json`: adds `ios3ds-wifid`, `ios3ds-nettest` and `com.ios3ds.wifi.plist`.
- `emu/qemu/hw/arm/nintendo3ds.c`: one `ctr_wifi_init()` call; `emu/build-qemu.sh`
  links `ctr_wifi.c` and configures QEMU with slirp.
- `Makefile`: `make test` also runs `make -C tools/ios3ds-wifi test`.

## 9. Open items

- Hardware bring-up (section 6); MCU Wi-Fi LED (I2C, optional).
- **Status-bar Wi-Fi icon: not feasible with this design.** SpringBoard gets
  Wi-Fi state from configd's `WiFiManager.bundle` (MobileWiFi), which only
  drives an `IO80211Interface`/`IO80211Controller` through the Apple80211
  ioctls ("Could not obtain IO80211Interface bsd name" in its strings). Our
  interface is an IOUserEthernet `en0`, so iOS networking works but the
  status bar shows no Wi-Fi bars and Settings > Wi-Fi stays empty. Showing
  them would need a kernel IO80211 controller emulating Apple80211.
- Data frames are sent with mailbox control word 0; dsiwifi uses `2008h` for
  IP traffic. Try that if unicast data does not flow after the handshake.
- TX flow control: no HTC credit accounting from the receive trailer yet;
  heavy traffic may overflow the firmware's MBOX ("Mbox Full" in dsiwifi).
- WPA (TKIP pairwise), WEP and enterprise networks are not supported.
- Faster path later: wake the daemon from the shim instead of 4 ms polling,
  CMD53 byte mode for BMI instead of one CMD52 per byte.
