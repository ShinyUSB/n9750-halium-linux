# Phase 5 report — modem DIAG investigation + WDS call comparison (2026-11-07)

Device: d2q, Debian (305-halium-boot-iparxdgb2, no D2QFIX, ns-lite v3).
Companion: ENDP-DESC-0x3F-ROOT-CAUSE-2026-11-07.md (addenda 1–4).

## 1. DIAG capture inventory and mask configuration

**Tooling built and working:**
- `diag_mdlog` (stock /vendor/bin, bionic) runs on Debian via
  `LD_LIBRARY_PATH=/vendor/lib64:/opt/bionic/system/lib64 /opt/bionic/system/linker64`.
  Three Debian-specific fixes were required:
  1. `chmod 666 /dev/diag` — diag_mdlog drops to uid 2000 (setuid(2000) confirmed
     by strace); the Debian node is root:root 0600 → EACCES.
  2. output dir must be world-traversable (uid 2000 cannot traverse /root 0700)
     → `/tmp/diagcap` chmod 777.
  3. a stale `/root/diagcap/diag_qsr4_guid_list.xml` also blocks (EACCES).
- `diag_cap.py` (custom, no bionic needed) — implements the kernel diagchar
  protocol reverse-engineered from `drivers/char/diag`:
  - write: `[pkt_type u32 = 0x20 (USER_SPACE_DATA_TYPE)][payload]`
  - log-mask command: `struct diag_log_config_req_t __packed {u8 0x73; u8 pad[3];
    u32 sub_cmd(3=SET_LOG_MASK); u32 equip_id; u32 num_items;} + mask bytes`
    (diag_masks.h:50); HDLC must be disabled first (`ioctl 38`, u8 0);
    logging mode must be MEMORY_DEVICE_MODE (`ioctl 7`,
    `diag_logging_mode_param_t{req_mode=2, peripheral_mask=0xffffffff,
    device_mask=1}`) — otherwise writes fail with EIO.
  - diagchar `poll()` is unreliable — blocking reads + SIGALRM required.
- `cap_full.so` — LD_PRELOAD shim (raw-syscall; captures AF_QIPCRTR
  send/recvmsg/send/write **and** /dev/wwan_ioctl WAN_IOC_ADD_FLT_RULE_EX);
  must live in `/vendor/lib64` (the vendor linker namespace rejects
  /data/local/tmp); rebuilt on the host with `clang -target aarch64-linux-gnu
  -fuse-ld=/usr/sbin/ld.lld` (no GNU aarch64 ld on the host; ld.lld works).

**Mask state:** the modem reports 26 equipped log-code ranges (134-200,
500-506, 1000-1007, … 10500-10505, 49152-49251); all masks were ALL-ZERO.
`diag_cap.py` sets every bit of every equipped range (equip_id = code/1000,
11 equips ≤ MAX_EQUIP_ID=16; equip 49 not settable) — **no log codes guessed**.

## 2. Decoded modem-side events

**None could be captured — see §3.** /dev/diag on Debian carries only the
kernel-local mask tables (the initial 21–31 KB burst on open) plus a 3–6 byte
response per mask-set command. No modem log packets ever arrive.

## 3. The blocker: the modem's DIAG transport is not connected on Debian

`/sys/kernel/debug/diag/rpmsginfo` (Debian, during a live call):
```
mpss: DIAG_DATA   inited: 0  opened: 0
mpss: DIAG_CTRL   inited: 0  opened: 0
mpss: DIAG_CMD    inited: 0  opened: 0      (and DCI_DATA/DCI_CMD: all 0)
```
Every diag glink channel toward the modem is uninitialized/closed. The modem
also never announced them: `/sys/bus/rpmsg/devices/` lists modem channels
DATA1/2/3/4/11, DS, IPCRTR, apr, SSM, LOOPBACK — **no DIAG_DATA/DIAG_CTRL**.
The kernel boot log shows the diag driver falling back to USB mode ("continuing
with USB registrations for proc: 0") and no DIAG channel events.

Consequences: the log-mask control packets (`DIAG_CTRL_LOG_MASK`) never reach
the modem, so full-mask logging produces nothing. **The modem-side DIAG view
(Q6 WAN pipe bring-up, UL-filter installation, DSD decisions) is not
observable from Debian** until this transport is brought up.

Open question: whether stock shows these channels open (it must, for Samsung's
on-device diag logging) — one stock command answers it:
`adb shell cat /sys/kernel/debug/diag/rpmsginfo`.

## 4. Stock vs Debian WDS initialization

Verified byte-identical (see addenda 1–4):
- WDS bind_mux_data_port: 15 B = TLV 0x11 (mux=1) + TLV 0x10 (EMBEDDED, iface 1)
- QMI_IPA handshake, WDA SET_DATA_FORMAT (71 B QMAPV4+agg), DPM open, DL rules
- APN="mts" → the same IP as stock (10.167.223.172)

Tested on Debian with **no effect** on the modem data path (bearer rx_ok=0,
producer-pipe filter table empty, UL boomerang, GET_DATA_STATS empty):
- call_type=EMBEDDED + profile_index_3gpp=1 + tech_pref=3GPP + DNS pref
  (the full TLV set matching stock's 38-B request size)
- WDS bind iface 0 / 1 / 2 (all bind successfully, same IP)
- DPM rx/tx order (both)
- 2-rule DL install (0-clear + v4/v6 catch-all) after the call

Stock start_network (38 B) decodes arithmetically to: APN(7) + ip_family(4) +
auth(4) + profile_idx(4) + call_type(4) + tech_pref(4) + autoconnect(4) +
DNS-pref(7). The auth value and profile index remain unverified (masked in the
Android log); capturing them needs one more stock session wrapping **rild**
(the WDS client) with cap_full.so — same procedure that captured ipacm.

## 5. Most likely root cause (with evidence)

The modem's **Q6 WAN pipes are never configured on Debian**. Evidence:
- The GSI ipc log shows only AP-side channels (ch 3/4/5/6/10) — **no Q6
  channel starts** (the modem's Q6_WAN_PROD/CONS would appear as GSI ch_ctrl
  from EE_Q6).
- The producer-pipe filter table is empty in HW (ip4_flt_hw) — the 13 Q6 UL
  rules are cached but neither the AP (gated by `qcom,modem-cfg-emb-pipe-flt`,
  present on stock too) nor the modem installs them.
- GET_DATA_STATS returns result-only (no pipes, no filters).
- Every AP→modem control-plane input is byte-identical to stock, so the
  divergence is inside the modem's data-path bring-up decision.

What makes the modem bring up its Q6 WAN pipes on stock but not Debian remains
unidentified. The two candidate mechanisms are (a) a WDS-call parameter still
missing (auth/profile values — capturable by wrapping rild on stock), or (b)
modem-internal state driven by the full Android telephony stack (DSD/profile)
that Debian does not reproduce. The DIAG transport outage blocks direct
observation of (b).

## 6. Smallest reversible fixes (in order of cost)

1. **One stock command** (no flash): `adb shell cat
   /sys/kernel/debug/diag/rpmsginfo` — establishes whether the DIAG
   transport is Debian-specific.
2. **One stock flash with the shim on rild** — captures the exact WDS
   start/bind bytes (auth value, profile index, call type, the 18 boot-time
   binds' iface numbers) → replicate byte-exact. The procedure is proven
   (ipacm capture); `stop vendor.ipacm`-equivalent is `stop ril-daemon` /
   restart rild wrapped. Recoverable by reboot.
3. **Kernel one-liner** (rebuild 307 + flash): in `handle3_egress_format`,
   drop the inner `!modem_cfg_emb_pipe_flt` gate so the AP installs the 13
   cached Q6 UL rules with their real equations. If the path activates, the
   flag was the operative difference; revert = reflash 305.
4. **DIAG transport bring-up** — determine why the modem doesn't announce
   DIAG_DATA/DIAG_CTRL (likely needs the stock comparison from 1 first).

## 7. Validation commands (once any fix is in)

```
# bring-up (Debian):
ip link set rmnet_ipa0 up
ip link add link rmnet_ipa0 name rmnet_data1 type rmnet mux_id 1
/root/rmnet-setup addmux 1 rmnet_data1
qmicli -d qrtr://0 --dpm-open-port="ctrl-ep-type=embedded,ctrl-ep-iface-number=1,ctrl-port-name=rmnet_ipa0,hw-data-ep-type=embedded,hw-data-ep-iface-number=1,hw-data-rx-id=11,hw-data-tx-id=2" --client-no-release-cid
python3 /root/wda_stock.py
/root/rmnet-setup egress 0x1e
/root/rmnet-setup ingress 0x3e 16384 31
ip link set rmnet_data1 up
/root/wds-call4 &              # full TLV set: APN+family+EMBEDDED+profile+tech
sleep 12
/root/dl-rule2                 # 0-clear + v4/v6 catch-all DL rules
ip addr add 10.167.223.172/32 dev rmnet_data1
ip route add default via 10.167.223.173 dev rmnet_data1 metric 700
ping -c 5 -I rmnet_data1 8.8.8.8        # success = 0% loss
grep STATS /root/wds3.log | tail -1     # success = rx_ok>0 AND tx_ok>0
timeout 4 cat /sys/kernel/debug/ipa/ip4_flt_hw 2>/dev/null  # ep_idx:2 rules present
```

## Artifacts

- `qmi-ipa-fix/endp-desc-analysis/diag_cap.py` — the DIAG mask+capture client
- `qmi-ipa-fix/endp-desc-analysis/cap_full.c/.so` — the extended shim
- `qmi-ipa-fix/endp-desc-analysis/wds-call3/4.c` — APN / full-TLV call clients
- `qmi-ipa-fix/endp-desc-analysis/dl-rule2.c` — the 2-rule DL sequence
- `qmi-ipa-fix/endp-desc-analysis/ipacm-qrtr2.cap` — the stock DL-rule capture
- phone: /root/{diag_cap.py,cap_full.so,wds-call4,dl-rule2,phase2.sh}
- phone state: /dev/diag chmod 666 (session), /vendor/lib64/cap_full.so (remove
  on next stock session), /proc/sys/kernel/kptr_restrict=0 (session)

---

# ADDENDUM 5 (2026-11-08, image 307v2 validation — the experiment result)

## Mission C experiment result: the hypothesis is REJECTED

307v2 (the gpu-exp tree + the D2Q307 egress patch, D2QFIX reverted) flashed and
booted Debian correctly (the D2QQMI handshake fired, no D2QFIX, no modem crash).

**The Mission C install worked exactly as designed:** after egress,
`ip4_flt_hw` shows the Q6 UL rules in hardware on the producer pipe:
```
ep_idx:2 rule_idx:0 act:0 rt_tbl_idx:2     (v4, meta/mux match)
ep_idx:2 rule_idx:1 act:0 rt_tbl_idx:2
ep_idx:2 rule_idx:2 act:0 rt_tbl_idx:1
ep_idx:2 rule_idx:3 act:0 rt_tbl_idx:1
ep_idx:2 rule_idx:4 act:0 rt_tbl_idx:1     (attrib 0x20 = dst-addr match)
```
The stock byte-exact WDS sequence (bind + bind_subscription + set_ip_family +
start_network with PAP/user/pass) then connected (the packet handle assigned).

**But the data path still does not flow:** UL frames (from the call's assigned
IP) boomerang to the LAN pipe; the bearer stats stay rx_ok=0/tx_ok=0; the
returning frames are our own queries echoed (not DNS responses); D2QRX=0.

## The decisive new finding: the modem's Q6 WAN GSI channels never start

The GSI ipc log (after the call) shows **only AP-side channels** starting
(ch 3/4/5/6/10 — the suspend/resume cycles). The modem's own WAN channels —
Q6_WAN_CONS (ep 13, receives UL) and Q6_WAN_PROD (ep 14, sends DL) — are
**never started by the modem's Q6**. Consequences (all measured):
- UL frames matching the rules route to ep 13 → the channel is not started →
  the IPA delivers them to the disable/default pipe (ep 10, LAN consumer) →
  they return on rmnet_ipa0 RX ("boomerang").
- GET_DATA_STATS: no active pipes.
- No DL (the modem's Q6_WAN_PROD also never started).

The route tables are fine: the modem's tables (tbl 1/2, referenced by the 13
rules) route to ep 13 (verified in ip4_rt_hw). The failure is purely that the
modem's Q6 does not bring its pipes up.

## Root cause (current best evidence)

The modem's Q6 data-path bring-up (starting the WAN GSI channels) requires a
modem-internal state transition that, on stock, is driven by the full Android
telephony stack — most plausibly the **DSD (Data System Determination, svc
0x1E) data-system status** and/or the DPM-driven port-to-pipe activation —
beyond the WDS call + QMI_IPA handshake + filter rules that Debian reproduces.
Every AP-side QMI exchange verified so far is byte-identical to stock; the
missing trigger is at the modem's data-system layer, not in anything the AP
sends on the WDS/QMI_IPA/DPM paths as currently replicated.

## Next experimentally justified step

Drive the modem's data-system state from Debian: speak to **DSD (svc 0x1E)**
(the DSD_SET_SYSTEM_STATUS / the data-system bind flow that the Android
telephony stack issues), and/or capture the stock DSD traffic (the same shim
procedure now proven on rild — the DSD client also lives in rild, so one more
wrapped-rild stock capture with the data call shows the complete DSD flow).
This is the one remaining layer between "the call is up" and "the Q6 pipes
start" that has never been observed from stock.

## Rollback/rollback state

The phone can be rolled back to 305 at any time (305 = the proven Debian boot).
The 307v2 image remains at /home/usb/artifacts/flash/307-halium-boot-embpipe-v2.img
(the gate patch + the D2QFIX reverted; the D2QQMI/D2QRX prints native).

---

# ADDENDUM 6 — CONSOLIDATED SESSION SUMMARY (2026-11-08)

## The complete causal chain of the Debian data-path failure

Layer-by-layer, what was verified equal to stock, what was fixed, and what
remains:

| layer | state on Debian | verdict |
|---|---|---|
| IPA firmware load (/dev/ipa) | fixed early (the systemd path unit) | OK |
| QMI_IPA service registration | ns-lite v3 | OK |
| QMI_IPA handshake (INIT_MODEM_DRIVER etc.) | byte-identical (the memory tables, the ranges, ctrl ep 11) | OK |
| 13 Q6 UL filter rules received+cached | identical | OK |
| DPM open port | succeeds (both rx/tx orders) | OK |
| WDA SET_DATA_FORMAT | stock 71-byte QMAPV4+agg replicated, accepted | OK |
| WDS bind mux (mux 1, EMBEDDED iface 1) | byte-identical (15 B) | OK |
| **BIND_SUBSCRIPTION (0xaf, id=1)** | **was missing — added in wds5** | fixed |
| **SET_IP_FAMILY (0x4d, IPv4) before start** | **was missing — added** | fixed |
| **START_NETWORK auth/user/pass** | **was missing — PAP mts/mts added** | fixed |
| rmnet ioctls (egress 0x1e / ingress 0x3e agg) | match stock | OK |
| DL filter rules (0x37) + notify round-trip | accepted by the modem | OK |
| **AP-side install of the 13 UL rules** | **image 307v2: now installed in HW** | fixed (307) |
| **modem Q6 WAN GSI channels (ep 13/14)** | **NEVER STARTED — the Q6 does not bring them up** | **OPEN** |

## The eliminated hypotheses (each with its disproving evidence)

1. ~~The missing 0x3f endpoint descriptor~~ — the stock never sends it; the
   "0x3f" traffic was UIM logical-channel ops (a decoder misattribution); the
   modem asserts on it (`Invalid msg_id: 63`) — D2QFIX/image 306 crashes the
   modem on every ingress ioctl. Reverted.
2. ~~The WDS call content~~ — the byte-exact stock sequence (BIND_SUBSCRIPTION,
   SET_IP_FAMILY, PAP auth, profile indices, EMBEDDED call type) connects with
   the same public IP and does not activate the path.
3. ~~The AP-side UL-rule installation~~ — 307v2 installs the 13 rules into the
   HW filter table (verified); the frames still boomerang, so the rules were
   never the operative gate on Debian.
4. ~~The WDS bind iface / DPM endpoint order / DL-rule content~~ — all varied,
   no effect.
5. ~~The DIAG transport~~ — identical to stock (the modem DIAG exits USB only;
   /dev/diag carries no modem logs on either system).

## The confirmed root cause

**The modem's Q6 does not bring up its WAN GSI channels (Q6_WAN_CONS ep 13 /
Q6_WAN_PROD ep 14) on Debian.** The GSI log shows only AP-side channel
activity. Everything the AP controls is stock-identical; the trigger that
makes the modem's Q6 start its WAN pipes is modem-internal and is not driven
by anything currently replicated. The strongest remaining candidate is the
**DSD (Data System Determination, svc 0x1E) layer** — the Android telephony
stack's data-system state machine, which on stock runs continuously (rild +
DSD clients) and which Debian does not reproduce at all.

## The next experiment (prepared)

One more wrapped-rild stock capture windowed over `svc data enable` — the same
proven `cap_full.so` procedure — to record the complete DSD/data-system flow
that precedes the Q6 pipe bring-up. Then replicate the DSD sequence in a
wds5-style raw client on Debian.

## Build environment (the complete recipe — rediscovered this session)

- The correct Debian-boot kernel source tree:
  **`/home/usb/ShinyKernel-gpu-exp/Kernel`** (the 305-era source; the
  D2QQMI/D2QRX prints are native; the a6xx-build/build-gpu.txt records it).
- The build dir: `/home/usb/mnt-sda/a6xx-build/out-gpu` (its .config is the
  live one — the stack protector was disabled for this build).
- The toolchain: the Snapdragon LLVM (clang 10.0.9) at
  `/home/usb/mnt-sda/android-kernel/Kernel/toolchains/Snapdragon-LLVM`, which
  the gpu-exp Makefile:381 hardcodes as
  `toolchain/llvm-arm-toolchain-ship/10.0` — restore that path as a symlink
  (the dir was deleted; the host lacks libtinfo.so.5 so the container is
  required to run the compiler).
- The build container: distrobox `kernel-build` (ubuntu 18.04) — python2
  symlinked (`sudo ln -sf /usr/bin/python2.7 /usr/local/bin/python2`).
- The compiler wrapper: `/home/usb/mnt-sda/a6xx-build/clang-a64.sh` — injects
  `--target=aarch64-linux-gnu` and strips `-fno-var-tracking-assignments`
  (which the Snapdragon clang rejects; the Makefile:811 cc-option leaks it).
- `scripts/gcc-wrapper.py` patched warn-only (the D2Q307 marker) — it fails on
  ANY compiler warning otherwise (the full rebuild surfaces benign ones).
- The out-gpu .config: CONFIG_CC_STACKPROTECTOR disabled (the cc-option check
  otherwise fails; the .config edit gets re-enabled by the kconfig sync if
  done before the first prepare — edit and build in one go).
- The pack: pack-d2q.sh with RAMDISK=/tmp/rd305.bin (extracted from 305 at
  page+ceil(ksize/page)*page, sha e0ca79b7…), RAMDISK_SHA overridden,
  CMDLINE_FILE=/tmp/cmdline305.txt, REFERENCE=305, KERNEL=the gpu-exp
  out-gpu Image-dtb. The kernel_size header diff [8-11] = legitimate.
- **The build gotchas: (a) the wrong tree boots Android; (b) the D2QFIX hunk
  must stay reverted in the gpu-exp tree (it crashes the modem); (c) the
  KSU_SUSFS C90 declarations in fs/open.c + fs/read_write.c needed hoisting
  in the android-kernel tree build only (the gpu-exp tree was clean).**

## Images

- 305 = the proven Debian baseline (rollback).
- 307v1 = WRONG TREE (booted Android) + D2QFIX — never flash.
- **307v2 = /home/usb/artifacts/flash/307-halium-boot-embpipe-v2.img
  (sha f9137a4c…) — the current experiment image: the gpu-exp tree + the
  D2Q307 egress gate patch, D2QFIX reverted, the D2QQMI/D2QRX prints native.**

## Session artifacts (qmi-ipa-fix/endp-desc-analysis/)

decode_full.py (the byte-order-correct qrtr_0 decoder), seven-0x3f-table.txt,
wda_stock.py (the stock 71-B WDA), wds-call3/4/5.c, wds5.py (the raw-QMI stock
sequence client), dl-rule2.c (the 2-rule DL), diag_cap.py (the DIAG mask+capture),
cap_full.c/.so (the QRTR+wwan_ioctl shim), rild-wds.cap/.txt (the stock WDS
capture+decode), ipacm-qrtr*.cap, stock-callsetup/ (the stock boot+call captures),
deb-qrtr0-current.log, dtb/ (the stock DTB + the dtbo dumps), the DIAG report.
