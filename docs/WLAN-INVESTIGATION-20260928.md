# d2q WLAN investigation log — 2026-09-28

## Hypothesis 0 (from README): missing `android,bcmdhd_wlan` DT node with GPIO 169
- Prior work added `wlan-en-gpio = <&tlmm 169 0>` to `sm8150.dtsi`.
- STATUS: WRONG. GPIO 169 belongs to the Qualcomm `qcom,cnss-qca6390`
  (QCA) node. It was copied to the Broadcom node by mistake.

## Evidence from working Android 15 (rooted adb, same ShinyKernel #1, Sep 16)
- PCI: `0000:01:00.0` = `14e4:4475` (Broadcom BCM4375) on RC0, GEN1 x1.
  BAR0 PA `0x60800000` (32K), BAR1 PA `0x60400000` (4M).
- Live DT `/soc/bcmdhd_wlan`: `wlan-en-gpio` = tlmm **65**,
  `wlan-host-wake-gpio` = tlmm **68**, pinctrl `bcmdhd_wlan_en` (gpio65)
  / `bcmdhd_default` (gpio68). Source: stock Samsung DTBO overlay
  (`sm8150-sec-d2q-chn-overlay-r*.dts`, fragment@99 -> &soc).
- Driver `101.16.87.1` = `bcmdhd_101_16` (built-in), fw `18.41.117`.
- GPIO 169 reads `in 0` even with working Wi-Fi: it is NOT the WLAN enable.
- PCIe RC0 DT (`sm8150-pcie.dtsi`: perst tlmm 35, same vregs/ep-latency)
  matches live Android DT exactly. RC0 config is NOT the problem.
- Firmware (live `/vendor/firmware`): `bcmdhd_sta.bin_b1`,
  `bcmdhd_clm.blob`, `nvram.txt_*` (11 variants). Driver uses
  `DHD_LINUX_STD_FW_API` + chip-suffix auto-select; Android HAL never
  writes `firmware_path`/`nvram_path` params (verified by polling across
  `svc wifi disable/enable`). All files pulled to
  `artifacts/wlan-firmware-android/` + SHA256SUMS.
- `firmware_class.path=/vendor/firmware_mnt/image` is a red herring for
  WLAN (modem/adsp files only); bcmdhd uses request_firmware names
  resolved from `/lib/firmware` (Debian) / `/vendor/firmware` (Android).

## Failure chain in Debian (kernel #2, Sep 27)
- Base DTB contained the bogus 169 node (no pinctrl, no host-wake).
- `dhd_wifi_init_gpio`: `gpio_wlan_power : 169`, `host_wake : -2`.
- PCIe RC0: `LTSSM_STATE: DETECT_QUIET`, `link initialization failed`,
  `No Broadcom PCI device enumerated!`.
- Firmware was never reached (correct: PCIe first, firmware second).

## Fix (minimal, reversible)
1. Removed the bogus `android,bcmdhd_wlan` node from
   `ShinyKernel/Kernel/arch/arm64/boot/dts/qcom/sm8150.dtsi`
   (hardlinked with `arch/arm/...`, one edit fixed both).
   DT is now byte-identical to the REVIEWED baseline; the stock DTBO
   overlay again solely provides the correct 65/68 node, exactly as in
   working Android #1.
2. Rebuilt `out-devtmpfs` incrementally in `kernel-build` container
   (config verified unchanged, BUILD_EXIT=0).
3. Verified both appended base DTBs: 0 occurrences of `bcmdhd_wlan`,
   `cnss-qca6390` node intact.
4. Packaged `artifacts/flash/51-halium-boot-wlanfix.img`:
   new Image-dtb + ramdisk/cmdline/board byte-identical to image 50.
   - sha256: dab27f41209ea7e0b419b5ff1b772a597aa555e20af1eca0c6247cedb6b42d76
   - recovery: images 47/50 preserved; stock boot.img preserved.
5. Firmware for Debian: `tools/install-wlan-firmware.sh` (dry-run default).

## RESULT (verified on-device, 2026-09-28, after reboot)
Flashed `51-halium-boot-wlanfix.img` (BOOT-only, heimdall). On the new kernel:

    dhd_wifi_init_gpio: gpio_wlan_power : 65
    dhd_wifi_init_gpio: gpio_request WL_REG_ON done - WLAN_EN: GPIO 65
    dhd_wifi_init_gpio: gpio_wlan_host_wake : 68
    msm_pcie_enable: PCIe RC0: LTSSM_STATE: LTSSM_L0
    msm_pcie_enable: PCIe RC0 link initialized
    /sys/bus/pci/devices/0000:01:00.0 = 0x14e4:0x4475

Firmware from /lib/firmware (driver 101.16.87.1, Chip 4375 Rev 5):

    Firmware: wl0: Jul 19 2022 10:52:21 version 18.41.117 (B1 Network/rsdb)
    FWID 01-1b4772fd        <- identical to Android's 18.41.117

Association / internet (SSID OpenWrt_1, ch6, -32 dBm):

    wpa_state=COMPLETED, WPA2-PSK, CCMP
    wlan0 192.168.2.50/24, default via 192.168.2.1
    curl http://deb.debian.org/ -> HTTP 200 ; http://1.1.1.1/ -> HTTP 301
    apt-get update over wlan0: OK
    17 BSS visible on scan (2.4 + 5 GHz)

Fully automatic after `reboot` (verified): d2q-wlan.service associates,
addresses and syncs the clock without any manual step. USB RNDIS kept as the
SSH recovery path and no longer carries the default route.

## DHCP: not offered by the AP (NOT a driver problem)
Both `busybox udhcpc` and `dhclient -d -v` send DHCPDISCOVER and receive no
OFFER; also with a different client MAC (rules out MAC filtering). Yet
192.168.2.1 answers ARP 3/3 and serves DNS, and static 192.168.2.50/24 gives
full IPv4 internet. So the OpenWrt SSID OpenWrt_1 simply has no DHCP server
reachable from this client. `d2q-wlan-up.sh` therefore tries DHCP first and
falls back to static 192.168.2.50/24 via 192.168.2.1 (override with
D2Q_WLAN_IP / D2Q_WLAN_GW / D2Q_WLAN_DNS).

Files added to the device:
  /usr/local/sbin/d2q-wlan-up.sh      (copy of tools/d2q-wlan-up.sh)
  /etc/systemd/system/d2q-wlan.service( copy of tools/d2q-wlan.service, enabled)
  /etc/wpa_supplicant.conf           (0600, OpenWrt_1)
  /lib/firmware/{bcmdhd_*,nvram.txt_*}
  /etc/apt/apt.conf.d/99force-ipv4    (router DNS returns AAAA only, no IPv6 route)
Masked: systemd-backlight (avoids the documented shutdown hang with panel blacked)
Disabled: wpa_supplicant.service (Debian's is Type=dbus, does not drive wlan0)

## 5 GHz band: verified separately
`WLAN_5G_346C` (5 GHz, ch36/5180 MHz) associated and completed with WPA2-PSK:

    wpa_cli status -> ssid=WLAN_5G_346C  freq=5180  wpa_state=COMPLETED
    iw dev wlan0 link -> signal -79 dBm, tx 866.5 Mbit/s
    DHCP: DHCPOFFER/DHCPACK 192.168.1.72 from 192.168.1.1  (bound)
    curl http://deb.debian.org/ -> HTTP 200

This also **confirms the DHCP finding**: the identical driver + supplicant
stack gets a lease instantly on this network and none on `OpenWrt_1`. The
missing DHCP server is a property of the OpenWrt SSID, not of bcmdhd.

Both networks are now in `/etc/wpa_supplicant.conf`:
  OpenWrt_1      priority 10 (2.4 GHz, -32 dBm, strong)  -> static map entry
  WLAN_5G_346C   priority  5 (5 GHz,  -79 dBm, 866 Mb/s) -> DHCP

`d2q-wlan-up.sh` tries DHCP first and only then consults the per-SSID static
map `/etc/d2q-wlan-static.conf`, so a static address can never be applied to a
network it does not belong to. (The earlier version had one hard-coded static
address; that would have applied 192.168.2.50 to the 192.168.1.x network.)

Post-reboot verification (clean `reboot`, nothing manual):

    kernel 4.14.190-ShinyKernel, WLAN_EN GPIO 65, pci 0x14e4:0x4475
    ssid OpenWrt_1, wpa_state=COMPLETED, 192.168.2.50/24 via 192.168.2.1
    RNDIS still 10.15.19.82/24 (recovery path intact)
    internet HTTP 200

## Open items
- Signal on the 5 GHz AP is weak here (-79 dBm). Works, but throughput will
  suffer; 2.4 GHz is the more reliable of the two in this spot.
- MAC is regenerated each boot (00:13:e0:... Liteon OUI, driver default).
  Factory MAC provisioning lives in Android EFS + `macloader`; not ported.
  This may matter for per-device network policy on the OpenWrt.
- WPA3/SAE not tested (needs a WPA3-only AP).
- If the DTBO overlay ever stops applying, put the correct node straight into
  `sm8150.dtsi` (gpio 65/68 + pinctrl refs) instead of relying on the overlay.
