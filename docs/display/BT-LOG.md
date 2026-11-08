# Bluetooth on SM-N9750 / BCM4375 — findings

Working dir notes for the internal BT adapter shared with Broadcom Wi-Fi.

## Hardware / firmware facts established

| Item | Value |
|---|---|
| BT UART | `/dev/ttyHS0` (QUP `c8c000`) — **not** `ttyHS8` |
| hciattach line | `hciattach -s 115200 /dev/ttyHS0 bcm43xx 115200 flow nosleep 11:22:33:44:55:66` |
| BDADDR | passed **positionally** as the last argument (not `-i`/env) |
| Valid BDADDR | `11:22:33:44:55:66` — an invalid one (e.g. `AA:AA:...`) disables RF entirely |
| Firmware that works | ROM firmware (no `.hcd` download) |
| Firmware that fails | `bcm4375B1_murata.hcd` (rev 0xab): answers digital commands, never does RF |
| NVRAM match | "BCM4375B1 Davinci 1RH JPN Ant0" (matches our unit) |
| Android reference | `/vendor/firmware/bcm4375B1_murata.hcd`, module `murata_mur_1rh_es43`, baud_1 4M / download 115200 / baud_2 4M, settlement 100 ms |
| UART baud accepted | 921600 and 115200 only; 4 Mbaud → status `0x12` invalid params |
| LE own_addr_type | must be `0x00` (public). `0x01` (random) hangs the ROM firmware |
| Controller HCI version | 4.1 → LL legacy pairing opcodes (`LL_ENC_REQ`=0x03 … `LL_START_ENC_RSP`=0x06) |

## Kernel work done

- `configs/d2q_bt.config` — enabled `BT_HCIUART` (+`SERDEV`/`H4`/`BCM`), `BT_HIDP`,
  `SERIAL_DEV_BUS`; all were off in the baseline.
- Force-public LE address patch in `hci_request.c:hci_update_random_address()`
  (image 62, sha256 `e19c71fa279af08af4eba97be4b27a3f2ee9af5ae93454406ffa90f5ed635809`).
  The ROM firmware hangs when asked to use a random LE address.

## The wedge (the central problem)

The controller answers commands for a short window after a power cycle and then
goes permanently silent. Reproduction facts:

1. **Digital commands work** right after a fresh `rfkill block` → `unblock`:
   `HCI_Reset` (`0x0C03`) → `04 0e 04 01 03 0c 00`, `Read Version` (`0x1001`) →
   `04 0e 0c 01 01 10 00 09 00 00 09 0f 00 11 11` (HCI 4.1, rev 0x1111).
2. **RF also works**: a raw LE scan found `08:AD:00:14:76:00` "Touch MM-997"
   (flags `01 05` = connectable + HID), and an **active** scan additionally
   returned the scan response `09 "Touch MM-997"`, which proves the TX path works.
3. Once something wedges it, `hciconfig hci0 up` still returns success but every
   subsequent command returns **`04 10 01 00` = HCI Hardware Error (0x10)**.
4. **UART loopback test discriminates the two failure modes**: in a wedged state
   `/sys/.../c8c000.qcom,qup_uart/loopback` returns nothing, i.e. the *host UART
   driver* is wedged, not just the chip. In a healthy state it echoes fine.
5. `dmesg` shows `[tty]geni_dump_dbg_regs: m_irq_status:0x0, geni_status:0x41000`
   and IRQ 472 stops incrementing — the QUP GENI UART stops servicing interrupts.
6. **QEP run 1: rfkill toggling does not always revive it.** After a wedge,
   `rfkill block bluetooth` → `rfkill unblock bluetooth` sometimes yields a chip
   that answers nothing at all (`reset NONE`, `ver NONE`). A reboot does.

## Host UART runtime PM

`/sys/devices/platform/soc/c8c000.qcom,qup_uart/power/` shows `control=auto`,
`autosuspend_delay_ms=150`. With an open FD the port re-suspends within 150 ms and
the chip stops answering. Setting `control=on` keeps `runtime_status=active` — but
something reverts it to `auto` later in the session (observed while an ancient
`bluetoothd` was still running). Keeping the port pinned on is necessary but was
not sufficient to prevent the wedge.

## Two discovery sub-channels (shared silicon)

- A saturated Wi-Fi TX flood (`busybox ping -i 0 -s 1400`) **unblocks TX** on the
  chip, which pointed at shared RF/clock resources between the BCM4361 WLAN and BT.
- Conversely, after BT wedges, **WLAN dies too** (`ping 192.168.2.1` 100% loss), so
  it is one shared crash, not a BT-only one. `bt_en` GPIO alone cannot reset it
  while WLAN holds the chip powered.
- Moving WLAN to 5 GHz (5260 MHz) to quiet the 2.4 GHz band let RX see the mouse again
  (one adv), but a further scan still saw almost nothing — consistent with a chip that
  is already degraded rather than a pure band-congestion problem.

## Mouse

"Touch MM-997", LE-only, `08:AD:00:14:76:00`, advertises fine on other phones.
It is alive (LED blinks when the pair button is held) — so any failure to see it is
ours, not its.

## Kernel/BlueZ path is unusable here

- `bluetoothd -d` reports "Number of controllers: 0" although `hci0` exists.
- mgmt `Set Powered` → `0x03`; discovery → `InProgress` / `NotReady`.
- `hci0` dies within seconds of `hciconfig hci0 up` (dmesg:
  `Bluetooth: hci0 command 0x200b tx timeout`, `0x200c tx timeout`).
- `bluetooth.service` frequently hangs in `deactivating` / `stop-post` and creates
  phantom systemd jobs; it is also `enabled`, so it grabs `/dev/ttyHS0` at boot.
- Unbinding `soc:bt_bluesleep` did not help.

## Cross-check: does the fault exist in Android too? (YES)

Tested on stock Android 15 (Halium build, `RF8M82H3NFM`) on the same unit.
This isolates hardware/Android from the Debian kernel work:

- Bluetooth service runs, `svc bluetooth disable/enable` succeeds, state `ON`.
- The HAL holds `/dev/ttyHS0` (`lsof` → `bluetooth@1.0-s` fd 7u).
- **`/sys/class/bluetooth/` is EMPTY** — no HCI device is ever created, so
  `hci_vendor` never enumerates a controller.
- Adapter address is the placeholder **`XX:XX:XX:XX:E0:AA`**, never a real MAC.
- Scan *appears* to run (LE commands 0x2041/0x2042 complete, `startDiscovery`
  logged, `deviceFoundCallback` fires) **but every "discovered" device also has a
  placeholder address**: `XX:XX:XX:XX:73:D4`, `XX:XX:XX:XX:FA:B0`. Two different
  devices, two different garbage addresses.
- `settings get secure bluetooth_bonded_devices` → `null` (see correction: this
  is NOT evidence of failure on this tree).
- **BUT** `dumpsys bluetooth_manager` shows `ConnectionState: STATE_CONNECTED`
  after the user paired the mouse in Settings.

**Correction (later the same session): the "broken hardware" reading above was
WRONG.** The user paired the mouse successfully in Android Settings, and
`getevent -pl` confirms a real HID device:

- `/dev/input/event11`, `name: "Touch MM-997       "`
- Full keyboard key bitmap (KEY_ESC…KEY_F12, modifiers, keypad) — it is a combined
  keyboard + mouse, not mouse-only.
- BUT it reports **no ABS_* or REL_* axes at all**, and `getevent` on the node
  produces **zero events**. So the HID node was created but no report map ever
  arrived: the device is paired/connected at the Bluetooth level but is not
  actually delivering pointer or key input.

Lesson: `/sys/class/bluetooth/` being empty and `bluetooth_bonded_devices` being
`null` are NOT reliable "BT is dead" signals on this tree — the stack works
despite them, and only `getevent`/real input proves anything.

**Net conclusion: the adapter is NOT hardware-broken.** RF, pairing and HID device
creation all work in Android. What is still unproven is pointer/key input, and what
remains unexplained is why the *Debian* side wedges seconds after
`hciconfig hci0 up`.

### Android pairing result — mouse WORKS

User paired the mouse in Android Settings and confirmed **the cursor moves and keys
type**. Bond record from `dumpsys bluetooth_manager`:

    shim::record 004 xx:xx:xx:xx:15:d5  BLE cod:0-5-80
      remote_info:09-00008-01294  name:"Touch MM-997       "
      bond_type:BOND_TYPE_PERSISTENT, bond_type:COMBINATION
      le_linkkey_known:T, ble_encrypted:T, le_authenticated:F, ble_enc_key_size:16

Note the bond is **`BOND_TYPE_COMBINATION` with `le_encrypted:T`** and a 16-byte
LE key — i.e. **LE Secure Connections** (SMP), *not* the LL legacy pairing that my
`bt-le-pair.py` implements. Also two older bonded devices exist (JBL Flip 6,
Redmi Buds 4 Active) from previous use.

All addresses are masked `xx:xx:xx:xx:..` because this Halium build masks the
first 4 octets in logs — so the `XX:` pattern is a **logging artifact, not a
controller fault**. Same applies to the adapter's own `XX:XX:XX:XX:E0:AA`.
Android's real own-address is `…:E0:AA`, i.e. **last 2 octets E0 AA**.

### Android BT vendor stack (the reference implementation)

- HAL: `/vendor/lib64/libbt-vendor.so`, UART `/dev/ttyHS0`.
- Firmware: `/vendor/firmware/bcm4375B1_murata.hcd`,
  md5 `2e3de2dc687dbac2c7a68afeebfe114a` — **identical** to our copy in
  `artifacts/bt-firmware/` (verified).
- Strings of interest: `HW_CFG_SET_UART_BAUD_2`, `bt vendor lib baud_1: set UART
  baud %i`, `bt vendor lib baud_2: set UART baud %i`, `UartBaudRate`,
  `sent module info for sub failed. set baud rate`, `.hcd` variants
  (`murata.hcd`, `murata_b1.hcd`, `murata_es1.hcd`, `semco*.hcd`, `wisol.hcd`).
- So Android sets **two** baud rates: `baud_1` before the download and `baud_2`
  after the chip has rebooted into the patched firmware.

### Debian step 1: loading the Android vendor patch — how far it gets

`hciattach` (BlueZ 5.66) does the patch download itself, but only if it finds the
file. It scans **`/etc/firmware`** (that exact dir, found via strace) and matches
entries against `.hcd`. Every earlier attempt printed `Patch not found, continue
anyway` because the symlinks there were never matched. Fix: place **real files**
(not symlinks) in `/etc/firmware`, e.g. `BCM4375B1_murata.hcd`.

Then it does start the download:

    Initialization timed out.
    bcm43xx_init
    Set Controller UART speed to 115200 bit/s
    Flash firmware /etc/firmware/BCM4375B1_murata.hcd

strace of the whole transfer (`/tmp/hs3.log`) shows it works perfectly right up to
the final step:

- **1255 `Write_RAM` (0xFC4C) writes, every one answered with status `00`.**
- Transfer rate is irrelevant: 175840 B at 115200 is only ~16 s of wire time, yet it
  stalls for the full `-t` timeout. So it is *not* a speed problem.
- Then exactly **one** `Launch` (0xFC4E) is written and answered with
  **`read(3, "\1N\374\0", 4)` → opcode 0xFC4E, status `0x01` = Unknown HCI Command**.
- The last accepted Write_RAM chunk carries offset bytes `6C 9C 0C 00`
  (little-endian 0x000C9C6C ≈ 826 KB into the stream) and data beginning
  `48 F7 EC C2 11 DF 8F 43...`.

**Conclusion: the entire 175 KB image is accepted into the chip's RAM and only the
final `Launch` is refused.** ROM firmware (the state Debian uses today) does not
implement vendor opcode 0xFC4E in the way the patch loader expects, so the patched
firmware can never be started. Note the *same* 0xFC4E/0x01 signature is what
"reset" returned right after a reboot earlier (`0e 01 03 0c 00` = CC, opcode 0x0C03,
status 0x01) — i.e. the ROM rejects the download-mode entry command as well.

Probing the ROM's vendor opcode set directly (0 params, status 0x01 = not
implemented, 0x12 = implemented/bad params):

    0x0C00 -> st=0x01 (and this probe WEDGES the chip)

0x0C00 is the Broadcom "download mode" entry command and the ROM does not have it.
So the patch route is genuinely closed on this ROM.

## ROOT CAUSE OF THE WEDGE — QUP UART runtime suspend (FIXED)

Two supporting observations first:

- `noflow` wedges exactly like `flow`, so hardware flow control is **not** the cause.
- UART stats during the wedge: `tx:532784 rx:30190`. The chip had absorbed 532 KB,
  so several earlier "wedges" were almost certainly *caused by* the failed patch
  download leaving the chip in a stuck state, not by RF or by BlueZ.

The real fix: **pin the QUP UART out of runtime PM.**

    /sys/devices/platform/soc/c8c000.qcom,qup_uart/power/control        -> on
    /sys/devices/platform/soc/c8c000.qcom,qup_uart/power/autosuspend_delay_ms -> -1

With `autosuspend_delay_ms=150` the port suspends 150 ms after the last transfer
*even while a file descriptor is open*, and this QUP GENI driver does not reliably
resume for BT traffic: `m_irq_status:0x0, geni_status:0x41000` and IRQ 472 stops
incrementing. Android's HAL never lets this happen, which is exactly why Android
works and Debian did not.

Result after pinning, with the patch removed from `/etc/firmware` (pure ROM path):

    hciconfig hci0  -> UP RUNNING
    HCI Version: 5.0 (0x9)  Revision: 0x0
    LMP Version: 5.0 (0x9)  Subversion: 0x1111
    Manufacturer: Broadcom Corporation (15)

and it stayed responsive for **4+ minutes** polling `Read Local Version` every 30 s,
versus dying within 60 s before. Note the ROM reports **HCI 5.0**, not 4.1 — so LE
Secure Connections (SMP over ATT, not LL legacy pairing) is the correct pairing path,
matching the Android bond record.

## Pairing the mouse on Debian — status

With the UART pinned, BlueZ works. Confirmed working:

- `hci0` stays `UP RUNNING` and answers `Read Local Version` indefinitely
  (verified 4+ min polling, and still UP after 12 min).
- LE scan finds the mouse repeatedly:
  `NEW Device 08:AD:00:16:46 Touch MM-997` (address rotates every advert —
  it uses a **resolvable private address**: seen `:46`, `:EA`, `:F0`, `:AD`,
  `:A3`, `:9B` across runs).
- Real addresses come back for other devices too (`7A:E8:E1:D7:FB:98`).
- `trust` succeeds, and **`Connection successful`** — the LE link comes up and
  the local name resolves to `Touch MM-997 `.
- GATT works: the device exposes **Battery Service / Battery Level**.

Remaining failure is the bond itself:

    Attempting to pair with 08:AD:00:16:A3
    Failed to pair: org.bluez.Error.AuthenticationRejected
    Failed to pair: org.bluez.Error.InProgress

Two separate causes found:

1. **`AuthenticationRejected` was a missing agent.** `bluetoothctl`'s own agent
   only registers *after* discovery starts (`Failed to register agent object`
   first, then `Agent registered` once `scan on` ran). Issuing `pair` without an
   agent fails immediately. Fixed by scanning once first so the agent registers.
2. **`InProgress`** is caused by discovery still running: `scan off` does not take
   effect promptly, and `pair` is rejected while a scan is active. Needs a long
   settle between `scan off` and `pair`.

A working pairing agent was also written (`artifacts/display/bt-agent.py`,
PyGObject/D-Bus `org.bluez.Agent1`, accepts Just Works). It registers the agent
object successfully; this PyGObject build's `call_sync` argument convention proved
awkward (`Must be string, not Variant` / `Expected GLib.Variant, but got str`),
so `bluetoothctl`'s agent plus correct ordering is the working path.

## SUCCESS — wireless mouse paired and working on Debian (2026-10-01)

**The mouse is bonded, connected, and delivering real HID input on Debian.**

Evidence (decoded from `/dev/input/event11`):

    events: 5027
    event types: {EV_SYN: 1809, EV_REL: 2590, EV_KEY: 320, EV_MSC: 308}
      REL_X      n=3201  max=79      <- pointer movement
      BTN_LEFT   n=119   max=2       <- left clicks (2 = release)
      BTN_RIGHT  n=173   max=2       <- right clicks
      REL_HWHEEL n=84    max=-3      <- scroll wheel

Kernel device:

    N: Name="Touch MM-997       "
    P: Phys=11:22:33:44:55:66
    U: Uniq=13:08:ad:00:19:f7
    B: REL=1c3   B: EV=12001f   B: LED=1f
    S: Sysfs=/devices/virtual/misc/uhid/0005:1235:AA22.0002/input/input12

Bond: `bluetoothctl devices Paired` -> `08:AD:00:19:F7 Touch MM-997`.

## The working recipe

Three separate things had to be true at once. Each one alone was not enough.

1. **Pin the QUP UART out of runtime PM** (the wedge cause, see above):
   `power/control=on` and `power/autosuspend_delay_ms=-1`.
   Re-apply it every time, because something reverts it to `auto`.
2. **ROM firmware, no patch**: `/etc/firmware` must be EMPTY, so `hciattach`
   prints `Patch not found, continue anyway`. (The patch cannot be launched:
   `Launch`/`0xFC4E` -> `0x01`.)
   `hciattach -s 115200 /dev/ttyHS0 bcm43xx 115200 noflow nosleep 11:22:33:44:55:66`
3. **Pair during an ACTIVE scan.** This is the non-obvious one. BlueZ evicts a
   device from its cache the moment discovery stops:
   `[DEL] Device ... Touch MM-997` -> any later `pair` says `not available`.
   And `pair` while scanning is rejected with `InProgress`. So pairing must be
   issued while `scan on` is still running. That combination succeeds.

    bluetoothctl
    > agent on
    > default-agent
    > scan on
    ...wait for "NEW Device .. Touch MM-997"...
    > pair 08:AD:00:xx:xx

   Result: `Connected: yes`, `Paired: yes`, `Pairing successful`.

Gotchas worth remembering:

- **Never issue a second `pair` after success.** Doing so returned
  `ConnectionAttemptFailed` and tore the bond back down.
- The mouse advertises under a **rotating resolvable private address**; observed
  `08:AD:00:16:xx`, `08:AD:00:17:xx`, `08:AD:00:18:xx`, `08:AD:00:19:xx`, and a
  stable-looking `08:AD:00:15:D5`. So capture the address from the scan output and
  use it immediately.
- `getevent` is NOT installed on this Debian; capture with
  `cat /dev/input/event11 > /tmp/ev.bin` and decode with `struct`. `input_event`
  on this 64-bit kernel is 24 bytes: **16 bytes of timestamp, then type(2),
  code(2), value(4)** — reading the type from offset 0 gives garbage.
- `btmgmt` is a dead end for pairing here: non-interactively it ignores the
  remote-address argument entirely (`Pairing with 00:00:00:00:00:00`) and it
  always forces BR/EDR, which the LE-only mouse rejects with `0x0d`.
- Bond dir is `/var/lib/bluetooth/11:22:33:44:55:66/` (named by our BDADDR), not
  `hci0/`.
## Boot persistence — NOT yet working (honest status)

The mouse was paired and working **in a live session**, but making that survive a
reboot has not succeeded. What a cold boot actually shows:

- `bt-mouse.service` starts and power-cycles BT, but **all 6 `hciattach` attempts
  fail** with `Initialization timed out`, and `/sys/class/bluetooth/` stays empty.
- With `bt-mouse` disabled (nothing touching BT at all), the chip is *still* mute
  straight after boot. So the service is not what breaks it.
- The **UART itself is healthy**: IRQ 472 increments (7470 → 7478), and
  `/proc/tty/driver/msm_geni_serial_hs` shows `tx:15165 rx:15155` moving. So the
  earlier "loopback DEAD = wedged driver" conclusion was wrong — loopback mode is
  unreliable here. The host side is fine.
- The chip simply **never answers**: `tx` creeps up a few bytes (hciattach's
  SET_BDADDR) while `rx` stays frozen at 15155 — nothing comes back.
- Repeated `rfkill block`/`unblock` cycles (4-6 of them, 6-15 s each) do **not**
  revive it. `[BT] Bluetooth Power On (300)` is logged every time, so `bt_en` is
  being asserted; some other rail/domain apparently is not.

Correction to an earlier note: rfkill cycling is **not** a reliable revival; it
worked in some sessions and not others. A reboot did reliably give a
*responsive* chip earlier in the session (that is how the successful pairing
happened), but it is not deterministic.

Files created for this:
- `bt-mouse.service` + `d2q-bt-mouse.sh` — attach/pin/reconnect (currently
  **disabled**, it does not succeed at boot)
- `bt-uart-pm.service` — pins UART PM at `sysinit.target` (enabled)
- `bt-uart-pm-guard.service` — re-asserts `power/control=on` (enabled)
- `bt-pair-pty.py`, `pair-mm997.sh`, `bt-agent.py` — pairing tools

Note: the early PM unit's `After=sysinit.target` + `Before=sysinit.target` is
self-contradictory; `power/control` still read `auto` at boot, so that unit is not
actually doing anything yet and should be fixed.

FIXED 2026-10-01 (post-reboot debugging): the on-device
`/etc/systemd/system/bt-uart-pm.service` still had the broken
`After=sysinit.target` / `Before=... sysinit.target ...` ordering, so it never
ran at boot (`inactive (dead)`, empty journal) — and `bt-uart-pm-guard.service`
(`Requires=bt-uart-pm.service`) never started either. Deployed the corrected
unit from `artifacts/display/` (`After=systemd-remount-fs.service`,
`Before=basic.target shutdown.target`); both units now `active`, UART shows
`control=on`, `autosuspend_delay_ms=-1`. Also fixed the stale
`MOUSE=08:AD:00:19:F7` (deleted RPA bond) in `d2q-bt-mouse.sh` — on-device and
in `artifacts/` — to the identity address `08:AD:00:15:D5`. Display/Weston
verified untouched and healthy (DSI-1 enabled @1440x3040, compositor running).

## Power-up debugging — what the chip needs to wake

Ruled out, with evidence, while trying to get it to wake at boot:

- **The UART/driver is healthy.** IRQ 472 (`msm_serial_geni0`) increments and
  `/proc/tty/driver/msm_geni_serial_hs` counters move (`tx:15165 rx:15155`).
  The earlier "loopback DEAD ⇒ wedged" reading was wrong — loopback mode is just
  unreliable here. What is frozen is the chip: `tx` creeps up (hciattach's
  SET_BDADDR) while `rx` never moves, i.e. the chip transmits nothing at all.
- **rfkill cycling does not revive it.** 4-6 cycles of 6-15 s each, with
  `[BT] Bluetooth Power On (300)` logged each time, all failed.
- **A long uninterrupted settle does not help** (75 s powered, then attach).
- **Not a baud-rate mismatch.** Probed the chip with `HCI_Read_Local_Version` at
  `B115200`, `B921600` and `B4000000` (Android's `baud_1` is 4 Mbaud) — all
  `NONE`. So it is not a 4 Mbaud-vs-115200 protocol mismatch either.
- **WLAN coexistence is not it.** WLAN was rfkill-blocked (`wpa_state=INTERFACE_DISABLED`,
  `ip link set wlan0 up` → "Operation not possible due to RF-kill"); after
  unblocking it and associating (freq 2432) the chip was still mute.
- **`bluesleep` is not it.** Tried fully unbound and re-bound (it drives
  `bcm,bt-wake-gpio` 0x40 / `bcm,bt-host-wake-gpio` 0x36); no effect.
- **There is no separate BT reset line or regulator.** The DTS `bt_driver` node's
  `bcm,bt-reset-gpio = <0x7f 0x0 0x0>` is consumed by `bcm4361.c` as its
  `bt_gpio_bt_en`, so it *is* gpio-300. `/sys/kernel/debug/gpio` shows only
  `gpio-300 (bcm4361_bten_gpio) out hi`, and `regulator_summary` contains nothing
  Bluetooth-related.

So `bt_en` is asserted, the UART is alive, and the chip still says nothing at any
speed. The missing step must be something Android does that we have not captured
yet — the `murata_mur_1rh_es43` module init, or its GPIO/power sequence. Next
step is to boot Android and capture the actual BT power-up to diff against ours.

(Note: when probing raw HCI, the baud argument must be a `termios.B*` constant,
e.g. `termios.B115200 == 4098`, never the literal `115200` — that gives EINVAL.
This bit me three times.)

Pairing works and HID input works, but the chip is not reliably wakeable at boot.
The realistic options are:

1. **Work out why the chip won't wake at boot** — compare a known-good boot's
   power state (other rails, `bcmdhd` WLAN coexistence, sleep GPIOs) against this
   one. Most likely the BT power sequence is incomplete and it only worked
   earlier by luck of ordering.
2. **Use a USB Bluetooth dongle** and sidestep the internal adapter entirely.
3. Bring BT up **on demand** after login (same steps that work interactively),
   accepting that it may need a retry loop that runs for minutes.

## What Android reveals (captured from the working reference)

Compared our boot-time sequence against Android's, GPIO-by-GPIO and
config-by-config:

- **BT power-up is identical.** Diffing `/sys/kernel/debug/gpio` across
  `svc bluetooth disable/enable` on Android shows exactly one line changes:
  `gpio-300 (bcm4361_bten_gpio) out lo` -> `out hi`. Nothing else. So the power
  sequence we do is the same one Android does, and it is not the missing step.
- **UART power management is identical too**: Android has
  `power/control=auto`, `autosuspend_delay_ms=150`, `runtime_status=suspended`
  - precisely our values. Android does not pin the UART either.
- **Android ends up at 4,000,000 baud** (`stty -F /dev/ttyHS0 speed` ->
  `4000000`), with `tx:923179 rx:7103` - the huge TX being the 175 KB patch
  download.
- Therefore the only substantive difference is the **firmware**: Android runs the
  patched image, Debian runs the ROM.

### The mouse's identity address (important!)

From `/data/misc/bluedroid/bt_config.conf`:

    [13:08:ad:00:15:d5]
    Name = Touch MM-997
    AddrType = 0            (public)
    LE_KEY_PENC = 332211665544998877221100554433662a518d6c631bdef030850110
    Service = 0000180f-... (Battery) 00001812-... (HID)

So the mouse's **stable identity address is `08:AD:00:15:D5`**; everything seen
while scanning (`08:AD:00:16:xx`, `17:xx`, `18:xx`, `19:xx`) are rotating
**resolvable private addresses**.

**Our Debian bond is stored under the wrong address.** BlueZ paired using whatever
RPA it happened to see and wrote the bond to
`/var/lib/bluetooth/11:22:33:44:55:66/13:08:AD:00:19:F7/info`. On reconnect the
mouse advertises as `08:AD:00:15:D5`, so BlueZ will never match the saved bond and
the mouse will never auto-reconnect. Writing the bond under `08:AD:00:15:D5`
should fix auto-reconnect.

Debian's stored `Key=33221166554499887722110055443366` is byte-identical to the
first 16 bytes of Android's `LE_KEY_PENC`, i.e. the LTK we derived is the same one
the mouse already trusts from Android.

Android adapter address for reference: `ec:aa:25:81:e0:aa`.

## The bond-address fix (applied, untestable while the chip is mute)

Rewrote the bond from the RPA address to the identity address:

    /var/lib/bluetooth/11:22:33:44:55:66/08:AD:00:15:D5/info
      Name=Touch MM-997
      Trusted=true
      [LongTermKey] Key=33221166554499887722110055443366
      EncSize=16  EDiv=12454  Rand=8074392419491388547

and deleted the five RPA bond dirs (`13:08:AD:00:16:9B`, `:16:AD`, `:16:F0`,
`:18:39`, `:18:EF`, `:19:F7`) so only one bond remains. This *should* let the mouse
auto-reconnect, but it could not be verified because the controller is mute on
this boot.

## Current blocking state

On this boot the controller is **completely dead**, and it is not a protocol or
configuration problem:
- `/proc/tty/.../msm_geni_serial_hs` reads **`rx:0`** — the chip has not
  transmitted a single byte since boot, through every attempt.
- `bt_en` (gpio-300) is asserted (`out hi`), the `bcm4361_bluetooth` driver is
  bound to `soc:bt_driver`, and `dmesg` shows the Power Off/On cycle.
- The live device tree is populated and has no missing BT node, and
  `/sys/kernel/debug/gpio` shows no BT line other than gpio-300.
- Tried, all leaving `rx:0`:
  - `hciattach` at 115200 and at 4,000,000 (Android's `baud_1`)
  - with `/etc/firmware/BCM.hcd` present, so the patch download path also ran —
    even SET_BDADDR got no reply, so the chip is not merely refusing `Launch`
  - with WLAN up (192.168.2.50, associated, 2432 MHz)
  - `bluesleep` bound and unbound
  - `bt_en` held **low for 60 s** then released, with a 20 s settle

The chip *did* work earlier the same day (that is how the pairing and the captured
HID input were obtained) with this exact power sequence, so this is intermittent
rather than a fixed misconfiguration. Nothing software-controllable here explains
it; the remaining suspects are a rail/clock some other component should enable, or
a hardware fault that has developed.

Practical options:

1. **USB Bluetooth dongle** — sidesteps the internal adapter entirely.
2. **On-demand bring-up** with a long retry loop, accepting some boots never
   come up.
3. Keep hunting the missing power step (would need a schematic or logic trace to
   identify which rail the chip actually needs).

## Current approach

Drive the controller entirely from raw HCI on `/dev/ttyHS0`, with **no** hciattach,
no BlueZ, no kernel BT stack: `artifacts/display/bt-le-pair.py`.
Implemented: H4 framing (HCI cmd/ACL/event), AES-128 (LE legacy pairing LTK),
`HCI_Reset` + `Set_BDADDR`, active LE scan with AD parsing, `LE Create Connection`,
`LL_ENC_REQ`→`LL_ENC_RSP`→`LL_START_ENC_REQ`→`LL_START_ENC_RSP`→`LE_Start_Encryption`,
LE LTK Reply, and HID report decode off the ACL channel.

Gotchas hit while writing it (worth keeping):
- `termios.B115200` is `0o10002`, **not** `115200`; passing the number → `EINVAL`.
- HCI responses get split across `read()`s — never parse one `read()` as one packet.
- AD length byte counts the type byte but not itself.

## Operational notes

- Always confirm `fuser /dev/ttyHS0` is empty before a raw test; stale holders
  corrupted many runs.
- `pkill -f <pattern>` can kill the agent's own SSH command — use explicit PIDs
  (`ps -o pid= -C name | xargs -r kill`).
- `ping` is absent on the phone; `busybox ping` exists.
- `cat` on some rfkill sysfs attributes on this device emits a huge stream of `01`
  bytes — redirect/head carefully.
- `bluetooth.service` must be **disabled** for the raw path to have the port.
## Post-reboot repair session (2026-10-01) — boot units fixed, chip still mute

Symptoms after reboot: `/sys/class/bluetooth/` empty, no `MM-997` input device,
`hciattach` → `Initialization timed out`. Display/Weston/WLAN/RNDIS verified
healthy and left untouched (DSI-1 enabled @1440x3040, compositor running).

Real bugs found and fixed:

1. **On-device `bt-uart-pm.service` never ran at boot.** It still had the old
   self-contradictory `After=sysinit.target` + `Before=... sysinit.target ...`
   ordering (`inactive (dead)`, empty journal). Deployed the corrected unit
   from `artifacts/display/` (`After=systemd-remount-fs.service`,
   `Before=basic.target shutdown.target`).
2. **`bt-uart-pm-guard.service` never started either** (`Requires=bt-uart-pm`
   could never be satisfied). Both units now `active` from boot; UART shows
   `control=on`, `autosuspend_delay_ms=-1`.
3. **Stale mouse address**: on-device `d2q-bt-mouse.sh` still had
   `MOUSE=08:AD:00:19:F7` (deleted RPA bond). Fixed to identity
   `08:AD:00:15:D5` on-device and in `artifacts/` (bond dir confirms only the
   identity bond remains; `/etc/firmware` empty = ROM path).
4. `enabled=disabled` on `card0-DSI-1/enabled` is a red herring (dpms attr);
   Weston owns the output and renders. Do not "fix" it.

Fresh reboot with the early pin active from sysinit: chip STILL mute —
`tx:0 rx:0` at boot, raw `HCI_Reset` → `NONE`, `hciattach` → `tx:8 rx:0` after
`rfkill block(8s)/unblock(12s)`. Also re-tried on the new boot: WLAN down,
`busybox ping -i 0` flood during attach. All `rx:0`, `bt_en` hi throughout.

`bt-mouse.service` deliberately left **disabled**: it cannot succeed while the
chip is mute, and an always-restarting power-cycle loop would only add noise.
Re-enable only after a manual attach is proven on a fresh boot.

Remaining triage: boot Android and check whether the chip answers there. If
Android BT is also dead → hardware fault, stop digging. If Android works →
Debian is missing an init step we have not captured.

## Android UART sequence captured (2026-10-01, working reference, mouse paired)

Fast-sampled `stty speed` + UART counters across `svc bluetooth disable/enable`:

- Host UART starts at **4,000,000 baud** (`baud_1`) within 0.2 s of enable.
- Bulk transfer at 4M (~+110K tx), then host drops to **115200** (~+75K tx —
  the `.hcd` download window), then back to **4,000,000** (`baud_2`) to run.
  Total ≈ 185 KB ≈ `bcm4375B1_murata.hcd` (175840 B, same size as our copy).
- Steady state: `4000000`, `tx:1110500 rx:17873`, modem lines
  `RTS|CTS|DTR|DSR|CD` — note **RTS asserted**. (Debian `hciattach noflow`
  attempts show `CTS|DSR|CD` with NO RTS — if the chip honors flow control,
  a deasserted RTS = "host not ready" = chip never transmits = `rx:0`.)
- Regulator diff off→on: use-count changes only, nothing powers up/down;
  `VDD_BTP_1P8` present in both states. GPIO diff: only gpio-300 lo→hi.

Leading hypotheses for Debian `rx:0`, in test order:

1. **Chip stuck in download-limbo, power-cycle not actually cycling.**
   Failed `hciattach` downloads (1255× Write_RAM then timeout) leave the chip
   expecting more records/a Launch; in that state it ignores HCI_Reset. Recovery
   requires bt_en to really go LOW. On Debian, verify `gpio-300` actually reads
   `out lo` after `rfkill block` — if it stays `hi`, every "power cycle" so far
   was void, including the 60 s hold. Fix: drive it via sysfs
   (`/sys/class/gpio` export 300, direction out, value 0), wait, then 1.
   Fits "worked once after a clean reboot, never again after failed downloads".
2. **RTS not asserted.** Force RTS high (`TIOCMSET`) before probing at 115200.
3. **Stale chip baud** (chip left at 4M by an earlier speed switch while host
   reopens at 115200): probe fresh-boot chip at B4000000/B3000000/B921600/B115200
   with correct `termios.B*` constants and raw HCI_Reset.

## ROOT CAUSE FOUND + FIXED (2026-10-01) — it was never power, it was pinctrl

**Root cause.** The QUP GENI UART leaves the BT pins in their pinctrl *sleep*
state the moment the last fd on `/dev/ttyHS0` is closed, and
`drivers/tty/serial/msm_geni_serial.c` never brings them back: at probe it only
*looks up* the states (`pinctrl_lookup_state`, lines 3473-3506) and there is no
`pinctrl_select_state()` call anywhere in the file. With gpio43/44/45/46
released, the BCM4375 is deaf — it never transmits, so every probe reads
`rx:0`.

**The experiment that proves it** (single fd, no close, RTS forced, 5 s each):

| state | result |
|---|---|
| port open, pins `function qup13` claimed | `B115200: 040e0401030c00` (HCI_Reset OK) |
| same open port at other speeds | `B921600/B2M/B3M/B4M: 00 00 …` (line noise only) |

and on the very first boot of a session, `hciattach` succeeds —
`Patch not found` → `Set BDADDR` → `Set Controller UART speed` →
`Device setup complete` → `hci0 UP RUNNING, RX bytes:712`. The instant anything
closes the port, `pinmux-pins` shows `MUX UNCLAIMED`, pinctrl state becomes
`sleep`, and the same probe returns `NONE` forever. That is why rfkill
cycling, 4 Mbaud, RTS, WLAN up/down, the firmware patch and 60 s power holds all
"did nothing": they all ran with the port closed.

**Fix — `d2q-bt-mouse.sh` + `bt-mouse.service` (enabled):**

1. `bt-uart-pm.service` pins the UART at sysinit (`control=on`,
   `autosuspend_delay_ms=-1`) — its own earlier ordering bug is fixed too.
2. Attach **once, early**, and never close it:
   `hciattach -t 30 -s 115200 /dev/ttyHS0 bcm43xx 115200 flow nosleep 11:22:33:44:55:66`,
   no rfkill power-cycle loop, no kill of hciattach.
3. `/etc/firmware` stays empty (ROM path; patch `Launch` 0xFC4E → 0x01).
4. `bluetoothd -n` run from that unit in a loop — the packaged
   `bluetooth.service` hangs on this image ("start operation timed out",
   "stop-sigterm timed out", phantom systemd jobs) and is left disabled.

**Verified cold boot, no manual steps:** `bt-mouse` active, `hci0` present,
`hciattach` and `bluetoothd` up, mouse **auto-reconnected**
(`Uniq=13:08:ad:00:1c:37`, `Connected: yes`), Weston logs
`event11 - Touch MM-997: device is a pointer`.

Input proven at kernel level: 402 events on `/dev/input/event11` —
`BTN_LEFT` 48, `BTN_RIGHT` 122, `MSC_SCAN` 76, `SYN` 156 (press + release).

**Pairing gotchas (cost hours, keep them):**
- The mouse advertises a **rotating RPA** (`13:08:AD:00:1B:06`, `:1C:37`,
  `:1C:57` …). A bond under one address will not match later. Keep a bond dir
  per address seen; BlueZ reconnects whichever it sees.
- `pair` must be issued **while a scan is running** (BlueZ evicts on scan stop),
  and `pair` right after `scan off` needs a long settle or it returns
  `not available`.
- `bluetoothctl` piped non-interactively sometimes prints nothing useful; use a
  FIFO: `mkfifo /tmp/bt.fifo; bluetoothctl </tmp/bt.fifo &`, then write commands
  to the FIFO and read the log. That is what finally produced `Connected: yes`.
- Do **not** rebind/unbind `c8c000.qcom,qup_uart` to "re-apply pinctrl": the
  remove path clears `uport->private_data`, so a later re-bind returns -ENODEV
  and `/dev/ttyHS0` never comes back until reboot.
- `bluetoothctl` in a pipe reports "Failed to register agent object" when the
  shell is not a TTY; the agent still registers and Just Works pairing succeeds.
