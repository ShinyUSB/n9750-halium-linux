# Stock N9750 netmgrd RMNET/IPA initialization — full recovery (2026-11-06)

Investigation of the stock N9750 vendor netmgrd/librmnetctl (pulled from the phone's
mounted /vendor) to recover the exact IPA pipe-setup sequence for Debian.
Ghidra project: /home/usb/mnt-sda/ghidra-proj/netmgrJ (netmgrd + librmnetctl64).
Binaries + dumps: camera-recon/netmgr/ (netmgrd.dis = full llvm-objdump with
.gnu_debugdata minisymtab extracted to mini.elf; netmgrd_decomp{,2..5}.txt).

## TL;DR — recovered stock values (msmnile target)

| op | payload | where |
|---|---|---|
| RTM NEWLINK | rmnet_data<i> on rmnet_ipa0, mux_id = i+1 (link0 → mux 1) | netmgr_rmnet_configure_embedded_link → rtrmnet_ctl_newvnd (librmnetctl RTM channel) |
| ADD_MUX_CHANNEL (0x89FD/5) | rmnet_ipa0, mux_id = i+1, vname = "rmnet_data<i>" | netmgr_kif_ifioctl_notify_mux_channel ← netmgr_kif_init_iface |
| WDA SET_DATA_FORMAT (QMI) | ll=raw-ip, qos=0, ul_proto=8 (QMAPV4), dl_proto=8, dl_pkt=31, dl_size=16384, ul_pkt=32, ul_size=16384 | netmgr_wda_set_data_format ← netmgr_qmi_set_data_format ← netmgr_qmi_service_init |
| RTM SETLINK VND | IFLA_RMNET_FLAGS flags=0xC000000D (see decode below); UL_AGG_PARAMS {size=16384, count=32} | netmgr_rmnet_change_embedded_links / update_embedded_ul_aggregation |
| SET_EGRESS_DATA_FORMAT (0x89FD/6) | u.data = **0x1e** (MAP\|AGGREGATION\|MUXING\|CHECKSUM) | netmgr_kif_ifioctl_set_egress_data_format ← netmgr_kif_set_data_format |
| SET_INGRESS_DATA_FORMAT (0x89FD/7) | u.data = **0x3e** (MAP\|DEAGG\|DEMUX\|CHECKSUM\|AGG_DATA), agg_size=16384 (0x4000), agg_count=31 (0x1f) | netmgr_kif_set_data_format |
| WDS BIND_MUX_DATA_PORT | mux-id = VND mux (1 for first link), ep-type=embedded, ep-iface-number=1 | data call time |
| WDS START_NETWORK | — | data call time |

**The librmnetctl proto-31 (RMNET_NETLINK) channel is DEAD on both kernels**
(stock d2q defconfig and ShinyKernel: `CONFIG_RMNET_DATA is not set` — no server).
Stock therefore uses `rtm_rmnet_data_enabled=1` (netmgr_main_cfg+0x8c0, default 1):
netmgr_rmnet_set_feature_flags (the proto-31 librmnetctl path) is SKIPPED; the
equivalent work goes over RTM (rtrmnet_*) and the 0x89FD ioctls.

## Call chain (all verified by address-level xrefs)

```
netmgr_main 0xa6744
 └─ netmgr_kif_init 0x7f1d0                      (netdev table)
 └─ netmgr_rmnet_configure_embedded_data 0x207ea8 (waits for rmnet_ipa0; loop ≤0x77)
 │    └─ netmgr_rmnet_configure_embedded_link 0x10a96c
 │         └─ rtrmnet_ctl_init + rtrmnet_ctl_newvnd  (RTM NEWLINK rmnet_data<i>, mux=i+1)
 │    (mux notify) netmgr_kif_init_iface 0x8c058 ← netmgr_kif_reset_link
 │         └─ 0x8d334: netmgr_kif_ifioctl_notify_mux_channel(x28+0x40="rmnet_ipa0",
 │                        x21="rmnet_data<i>", mux_id = i+1)
 │              = ioctl(0x89FD) {ext=5, mux_id, vchannel_name}  [only if GET_FEATURES bit0]
 └─ netmgr_qmi_service_init 0xbedc4  (on IPA QMI service up; per link)
      └─ netmgr_qmi_set_data_format 0xc3b34
           ├─ netmgr_wda_set_data_format 0x212300   (WDA SET_DATA_FORMAT to modem)
           ├─ [rtm_rmnet_data_enabled=1] netmgr_rmnet_change_embedded_links(w8=0xC000000D)
           │     └─ rtrmnet_ctl_changevnd per VND (RTM SETLINK IFLA_RMNET_FLAGS)
           └─ netmgr_kif_set_data_format("rmnet_ipa0", idf=0x4e, edf=0x2e)
                 ├─ features bit1 → netmgr_kif_ifioctl_set_egress_data_format(dev, 0x1e)
                 │     = ioctl(0x89FD) {ext=6, u.data=0x1e}
                 └─ features bit2 → ingress {ext=7, u.data=0x1e|0x20=0x3e,
                        agg_size=cfg+0x1b0=16384, agg_count=cfg+0x1b4=31}
```

## Flag computation proof (netmgr_qmi_set_data_format 0xc3b34)

- Config keys (netmgr_config_load → netmgr_main_cfg offsets):
  cfg+0x1b0 = dataformat_agg_dl_size = 16384; cfg+0x1b4 = dataformat_agg_dl_pkt = 31;
  cfg+0x1cc = dataformat_agg_ul_pkt = 32; cfg+0x8c0 = rtm_rmnet_data_enabled (default 1);
  cfg+0x8c8 = dl_marker_enabled = 1; cfg+0x8d8 = rmnet_shs = 0; cfg+0x910 = uplink_priority = 0;
  cfg+0x192 = ibfc (NOT set for msmnile → default).
- Jump table 1 (DL agg proto = [sp+0x84] granted == [sp+0x2c] requested = 8):
  proto8 → target 0xc579c keeping defaults **w19=4 (tailspace), w20=0x4a**.
  (proto5 → w19=0/w20=0x0a; proto7 → w19=4/w20=0x2a; proto9 → w19=0x20/0x30, w20=0x4a)
- IBFC (cfg+0x192=0): w21 = w20 = 0x4a; w23 = w19 = 4.
- Jump table 2 (UL agg proto = 8) → 0xc6064: cfg+0x1cc=32>0 → **w20 = 0x2e**,
  w8 = 0x80000009. (ul proto 5 → w20=0x0a; 7 → w20=0x1a — NOT the msmnile case)
- w21 |= 4 → 0x4e.  w8 |= w23|dl_marker(0x40000000) → 0xC000000D.
- convert_edf(0x2e) = (f&0xe)|((f&0x30)?0x10:0) = **0x1e**.
  convert_idf(0x4e) = (f&0xe)|((f&0x60)?0x10:0) = **0x1e**, then |AGG_DATA(0x20) → **0x3e**.

## Phase 2 — flags vs local kernel UAPI (include/uapi/linux/msm_rmnet.h)

Local header: EGRESS MAP(1<<1) AGGREGATION(1<<2) MUXING(1<<3) CHECKSUM(1<<4) IP_ROUTE(1<<5);
INGRESS MAP(1<<1) DEAGGREGATION(1<<2) DEMUXING(1<<3) CHECKSUM(1<<4) AGG_DATA(1<<5) IP_ROUTE(1<<6).
- egress 0x1e = MAP|AGGREGATION|MUXING|CHECKSUM — all interpreted by handle3_egress_format:
  CHECKSUM → hdr_len=8, cs_offload_en=IPA_ENABLE_CS_OFFLOAD_UL, cs_metadata_hdr_offset=1;
  AGGREGATION → aggr_en=IPA_ENABLE_DEAGGR, aggr=IPA_QCMAP (UL agg in the IPA TX pipe);
  → ipa3_setup_sys_pipe(APPS_WAN_PROD) → **apps_to_ipa3_hdl**; egress_set=true.
- ingress 0x3e = MAP|DEAGG|DEMUX|CHECKSUM|AGG_DATA — handle3_ingress_format:
  CHECKSUM (ipa_hw_type >= v4_5 — **this device: hw_type=15 = IPA_HW_v4_5**, read live from
  /sys/kernel/debug/ipa/hw_type) → cs_offload_en=IPA_ENABLE_CS_DL_QMAP, hdr_len=8,
  dl_csum_offload_enabled=true; AGG_DATA → aggr_byte_limit=16384, aggr_pkt_limit=31
  (+ ipa_disable_apps_wan_cons_deaggr(16384,31));
  → ipa3_setup_sys_pipe(APPS_WAN_CONS) → **ipa3_to_apps_hdl**, plus ipa3_setup_a7_qmap_hdr()
  + ipa3_setup_dflt_wan_rt_tables().
- The generic 0x0A/0x0E defaults are the no-checksum variants; stock msmnile = +CHECKSUM
  both directions + UL AGGREGATION + DL AGG_DATA. This matches QMAPV4 (WDA proto 8) with
  DL checksum offload — consistent with IPA 4.5's IPA_ENABLE_CS_DL_QMAP path.
- RTM port data_format 0xC000000D = bit31 RMNET_EGRESS_FORMAT_AGGREGATION (rmnet core UL agg)
  | bit30 INGRESS DL_MARKER_V1 (dl_marker_enabled=1) | bit3 DEMUXING | bit2 DEAGGREGATION
  | bit0 (as computed; MAP bit1 only if ibfc=1 — msmnile ibfc default 0).
  Replaces the `ip link add` default (IP_ROUTE|IP_ROUTE = bits 24|25) which would send RX to
  rmnet_ip_route_rcv (no QMAP demux) and TX without a QMAP header (dropped by ipa3_wwan_xmit's
  "SW filtering out none QMAP packet").

## Phase 3 — kernel init conditions (local source rmnet_ipa.c, verified)

- **ipa_config_is_apq**: never assigned anywhere in the tree → false. The APQ early-return in
  handle3_egress_format (which would skip pipe creation) never fires.
- **mux id 1 already registered?** No: mux_channel[] is populated ONLY by ADD_MUX_CHANNEL;
  none ran this boot (rmnet_data1 was created via RTM only). ipa3_find_mux_channel_index(1)
  = MAX → not a dup; the handler's "already setup mux" path not taken.
- **Child name**: cached verbatim (vchannel_name); dup check is by mux_id only.
- **num_q6_rules == 0 right now**: no QMI_IPA_INSTALL_FLTR_RULE_REQ seen this boot
  (IPA ipc msg log has no install_flt lines; handler = ipa_qmi_service.c:161 → only CACHES
  rules into num_q6_rules). Same state as stock at netmgrd init time.
- **ipa3_wwan_register_to_ipa timing**: DEFERRED. ADD_MUX_CHANNEL with num_q6_rules==0 &&
  !is_apq → caches the channel (mux_channel_set=true, ul_flt_reg=false), does NOT register
  to IPA. On this kernel the later batch-register (ipa3_wwan_update_mux_channel_prop) has no
  callers (dead code, same in the stock tree) — the registration is for tethering/offload
  extras (A2 QMAP hdr, tx/rx intf props); the embedded data path does not need it:
  TX = ipa3_wwan_xmit → ipa3_tx_dp(APPS_WAN_PROD) direct (needs: pipe + ETH_P_MAP skb);
  RX = apps_ipa_packet_receive_notify → rmnet_ipa0 (ETH_P_MAP) → rmnet core demux by mux_id
  → port->muxed_ep[1] = rmnet_data1 (needs: RX pipe + port format w/o IP_ROUTE + DEAGG).
- **egress_set**: false now; set true by SET_EGRESS. Gates UL-rule install at egress time
  (skipped while num_q6_rules==0 — "no UL-rules").

## Exact ioctl mechanics (from netmgrd disasm + kernel source)

socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, 0); ifreq{ifr_name="rmnet_ipa0",
ifr_data=&struct rmnet_ioctl_extended_s}; ioctl(fd, 0x89FD, &ifr).
Kernel: ipa3_wwan_ioctl (ndo_do_ioctl of rmnet_ipa0), CAP_NET_ADMIN required,
copy_from_user of the full 24-byte struct rmnet_ioctl_extended_s.

Payloads (LE):
```
ADD_MUX_CHANNEL:   05 00 00 00 | 01 00 00 00 | 72 6d 6e 65 74 5f 64 61 74 61 31 00 00 00 00 00
SET_EGRESS:        06 00 00 00 | 1e 00 00 00 | (16 bytes zero)
SET_INGRESS:       07 00 00 00 | 3e 00 00 00 | 00 40 00 00 | 1f 00 00 00 | (8 bytes zero)
```

Expected rc = 0 for all three. Expected kernel log (IPAWANDBG → pr_debug + IPA ipc log;
enable via dynamic debug or read /sys/kernel/debug/ipa/msg):
- "get ioctl: RMNET_IOCTL_EXTENDED"
- "ADD_MUX_CHANNEL(1, name: rmnet_data1)" / "cashe device[rmnet_data1:1] in IPA_wan[0]"
  / "dev(rmnet_data1) haven't registered to IPA"   (deferred branch — expected!)
- "get RMNET_IOCTL_SET_EGRESS_DATA_FORMAT 1e" / "UL chksum set" / "WAN UL Aggregation enabled"
  / "no UL-rules"
- "Get RMNET_IOCTL_SET_INGRESS_DATA_FORMAT" / "DL chksum set" / "get AGG size 16384 count 31"
- errors (if any) via IPAWANERR → always in dmesg.

## WDA SET_DATA_FORMAT (stock, sent to the modem BEFORE the ioctls)

netmgr_wda_set_data_format struct → QMI WDA SET_DATA_FORMAT (msg 0x0020):
ll_prot=raw-ip(+0x04), qos=0(+0x08), ul_agg_proto=8(+0x0c), dl_agg_proto=8(+0x14),
dl_pkt=31(+0x24), dl_size=16384(+0x2c), ul_pkt=32(+0x50), ul_size=16384(+0x58),
qos_hdr(+0x40), dl_pad(+0x48), fc(+0x4d), tcp_coal/udp_coal(+0x5d/0x5e).
QMI WDA agg proto enum (v01): 5=QMAP, 6=QMAPV2, 7=QMAPV3, 8=QMAPV4, 9=QMAPV5.
NOTE: qmicli 1.32.2 max is qmapv5(=6) — cannot send 8; needs a small QMI/QRTR helper
(service WDA=0x1e) if we reproduce this step byte-exactly.
**Why it matters**: ingress CHECKSUM makes IPA expect 8-byte QMAP DL headers with csum;
if the modem is never told QMAPV4+csum (WDA), DL frames arrive mis-framed. On stock this
message is what puts the modem in that mode. (If WDA is skipped on Debian, drop the
CHECKSUM bit from ingress → 0x2e... i.e. ingress u.data = 0x1e without 0x10, and egress
without 0x10 → 0x0e; matches the "generic reference" 0x0A/0x0E+agg bits.)

---

# PHASE 1-4: the stock QMI transport (option A) — 2026-11-06

## Phase 1 — DATA5_CNTL is NOT a physical channel

- netmgr_config msmnile: `single_qmux_ch_enabled=1, single_qmux_ch_conn_id_str=rmnet_ipa0,
  single_qmux_ch_name=DATA5_CNTL` — but this names a *named-QMUX channel*, and
  **`qmi_cci_qmux_xport_register` in /vendor/lib64/libqmi_cci.so is a STUB returning -1**
  (disassembled: the whole function is `return -1`). The named-QMUX path is dead code.
- No SMD/GLINK endpoint named DATA5_CNTL exists. Live survey: GLINK modem channels =
  DATA1/DATA2/DATA3/DATA4/DATA11/DS/IPCRTR/LOOPBACK_CTL_MPSS/SSM_RTR/apr_*/glink_ssr —
  no DATA5*. /dev/smd{4,7,8,11} + /dev/smdcntl8 exist (legacy SMEM SMD, unused for QMI
  on this platform).

## Phase 2 — the actual stock transport = QRTR (AF_QIPCRTR)

libqmi_cci.so has exactly two xport ops tables:
- `qcci_ipc_router_ops` → open does `socket(27 /*AF_MSM_IPC*/, SOCK_DGRAM|CLOEXEC, 0)`
- `qcci_qrtr_ops`       → open does `socket(42 /*AF_QIPCRTR*/, SOCK_DGRAM|CLOEXEC, 0)`
Selection: `qmi_cci_xprt_qrtr_supported()` → qrtr if supported (else ipc_router).
- Stock d2q_chn_hk_defconfig AND running ShinyKernel: `CONFIG_QRTR=y`, **no
  CONFIG_QCOM_IPC_ROUTER** → AF_MSM_IPC socket(27) fails (verified on the phone:
  Errno 97). So on stock, netmgrd's CCI uses the **AF_QIPCRTR/QRTR transport — the
  byte-identical wire to our libqmi attempt** (7-byte QMI header [type,txn,msg,len] + TLVs).
- The "QMUX framing" (cntl_flag/txn_id/msg_id/msg_len/svc_id seen in CCI log strings) is
  the CCI-internal header used on the AF_MSM_IPC socket only; over QRTR the frame is the
  raw QMI message — exactly what our libqmi sent.

## Phase 4 — how Samsung's CCI gets its channel on SM8150

It doesn't get a QMUX/SMD channel: it uses the QRTR socket. The DATA5_CNTL config string
is a legacy vestige (stub register). CONCLUSION: **the InvalidOperation is NOT a transport
problem — the wire was already correct.**

## The remaining stock delta = content/state, not channel

Stock netmgrd's WDA request over the same QRTR wire succeeds (proven: the pipe ioctls are
gated on WDA success in netmgr_qmi_set_data_format, and stock data works). Deltas between
our attempt and stock, in decreasing likelihood:
1. **flow_control TLV (=1)**: stock includes it (qos_enabled=1 [qos_disabled absent → 0 →
   bit2 set, "Setting QOS enabled" log path], qos==0, ibfc=0 → fc_valid=1, fc=1).
   libqmi 1.32 has NO input setter for flow_control (or dl_minimum_padding) — our request
   omitted it. Candidate TLV id 0x1A (CAF wda_v01.h ordering; pin at implementation time).
   NOTE: dl_minimum_padding=0 was also stock-included (no-op vs omission).
2. Session/timing: netmgrd runs WDA from netmgr_qmi_service_init with its full QMI client
   context; possibly the modem's embedded-port state only accepts the format in a window
   netmgrd hits at boot.
3. The QRTR WDA instance (svc 0x1A) may be a limited one (it also failed
   GET_SUPPORTED_MESSAGES with InvalidQmiCommand) — but stock uses the same instance.

## Tooling produced (camera-recon/netmgr/, phone /root/)

- rmnet-setup (0x89FD ioctls), rmnet-rtm (RTM flags+UL agg), wda-setdf (libqmi WDA),
  cap_qrtr.so (LD_PRELOAD AF_QIPCRTR hex capture). Raw capture: netmgr/wda-raw-final.log.
- Ghidra project ghidra-proj/netmgrJ: netmgrd + librmnetctl64 + libqmi_cci analyzed.
  Dumps: netmgr/netmgrd.dis (full llvm-objdump), netmgrd_decomp{,2..5}.txt, cci_decomp.txt,
  rmnetctl64_decomp.txt, rtrmnet64_decomp.txt, rmnetctl64_fill.txt; mini.elf = netmgrd
  minisymtab (name→vaddr).

## Recommendation

The stock-faithful WDA step reduces to: same QRTR channel + the byte-exact request WITH
flow_control=1 (and dl_minimum_padding=0) — one extra TLV libqmi can't express. If that
still returns InvalidOperation, the cause is modem session/timing and the pragmatic path is
the no-WDA variant (egress 0x0e / ingress 0x2e + plain-QMAP modem default framing).

---

# WDA TLV IDs — VERIFIED against the local stock IDL (libqmiservices.so)

WDA SET_DATA_FORMAT request element-info table @ file offset 0x8089 in
/vendor/lib64/libqmiservices.so (entries [typeA, tlv, extra, struct_offset],
struct offsets cross-validated against netmgrd's request-struct map):

| field | struct off | TLV id | wire type |
|---|---|---|---|
| qos_format | 0x01 | 0x10 | u8 |
| link_layer_protocol | 0x04 | 0x11 | u32 |
| ul_data_aggregation_protocol | 0x0c | 0x12 | u32 |
| dl_data_aggregation_protocol | 0x14 | 0x13 | u32 |
| ndp_signature | 0x1c | 0x14 | u32 |
| dl_data_aggregation_max_datagrams | 0x24 | 0x15 | u32 |
| dl_data_aggregation_max_size | 0x2c | 0x16 | u32 |
| endpoint_info | 0x34 | 0x17 | struct{u32 ep_type, u32 iface} |
| qos_header_format | 0x40 | **0x18** | u32 |
| dl_minimum_padding | 0x48 | **0x19** | u32 |
| flow_control | 0x4d | **0x1A** | u8 |
| ul_data_aggregation_max_datagrams | 0x50 | 0x1b | u32 |
| ul_data_aggregation_max_size | 0x58 | 0x1c | u32 |

Corrections vs my first draft: dl_minimum_padding = **0x19** (I had wrongly used 0x18,
which is qos_header_format); flow_control = **0x1A** confirmed (matches the published
legacy schema). No tcp/udp coalescing fields exist in this message (table ends at 0x1c).
Per instruction, dl_minimum_padding is OMITTED (0 = modem default).

## Final request (71 bytes) — helper: netmgr/wda_raw.py

```
00 01 00 20 00 40 00                                          hdr: req, txn1, msg 0x0020, len 64
1c 04 00 00 40 00 00                                          ul_max_size      = 16384
1b 04 00 20 00 00 00                                          ul_max_datagrams = 32
17 08 00 04 00 00 00 01 00 00 00                              endpoint_info    = {EMBEDDED, 1}
16 04 00 00 40 00 00                                          dl_max_size      = 16384
15 04 00 1f 00 00 00                                          dl_max_datagrams = 31
13 04 00 08 00 00 00                                          dl_agg_proto     = QMAPV4 (8)
12 04 00 08 00 00 00                                          ul_agg_proto     = QMAPV4 (8)
11 04 00 02 00 00 00                                          link_layer       = RAW_IP (2)
1a 01 00 01                                                  flow_control     = 1
```
Byte-diff vs the captured (modem-answered) libqmi request: only the header len field
(0x3c→0x40) + the appended fc TLV. Everything else byte-identical.

---

# WDA over QRTR — FINAL RESULT (2026-11-06): InvalidOperation(70) persists

The byte-exact, TLV-verified request (fc=1 @0x1A confirmed from the local stock IDL)
over the proven-correct QRTR wire:
- NS resolved WDA(0x1A) -> node 0 port 0x38 (56) via ns-lite.
- Request: 71 bytes (see above). Response: `02 01 00 20 00 07 00 02 04 00 01 00 46 00`
  = FAILURE / error 0x0046 (InvalidOperation).

CONCLUSION: the rejection is NOT transport (QRTR is stock's wire — proven), NOT content
(byte-exact + TLV-verified incl. fc=1). It is modem-side session/state: the QRTR-exposed
WDA instance has no data-port context, or the modem's embedded port is in a state that
rejects format changes at this point in the boot/session. This is not recoverable from
userspace read-only analysis; the decisive evidence would be a stock-boot strace of
netmgrd's WDA exchange (requires a stock flash) or MPSS-side logs.

## Pragmatic path forward (option b)

Skip WDA; use the no-checksum ioctl variants matching the modem's default plain-QMAP
framing: SET_EGRESS u.data=0x0e (MAP|AGGREGATION|MUXING), SET_INGRESS
u.data=0x2e (MAP|DEAGG|DEMUX|AGG_DATA) + {agg_size=16384, agg_count=31}.
The kernel then uses hdr_len=4 (plain QMAP), no csum offload; DL aggregation stays
modem-side-off. Data flows; QMAPV4 checksum offload can be layered later if the WDA
gate is ever cracked via a stock-boot capture.

State preserved: only the inert ADD_MUX_CHANNEL cache entry; no pipes/ioctls/WDS done.

---

# Option B execution — Stage B1 result (2026-11-06)

RTM SETLINK on rmnet_data1 (flags=0xC000000D, mask=0xC000000D, UL_AGG={16384,32,0,2987712})
→ **EBUSY (-16), "some changes committed already"**. Wire message verified structurally
correct (dry-run dump): ifi_index=13 (rmnet_data1), IFLA_LINK=12 (rmnet_ipa0),
LINKINFO{kind=rmnet, data{FLAGS={0xC000000D,0xC000000D}, UL_AGG_PARAMS={16384,32,0,2987712}}}.

State analysis:
- The rmnet_changelink path has NO EBUSY (returns 0/-ENODEV only). Kernel dispatch for an
  existing dev = ops->changelink THEN do_setlink on the same message.
- **The port data_format change DID take effect** (proven behaviorally: TX on rmnet_data1
  now reaches rmnet_ipa0 as QMAP-framed and dies at the dead pipe with "pipe is not valid"
  — with the old IP_ROUTE format it would have been dropped earlier as "none QMAP packet").
- The EBUSY origin is in the generic do_setlink tail on this Samsung CAF fork (not pinned
  to a specific line — no EBUSY in rtnetlink.c/rmnet_*; possibly a fork-local hook).
- UL_AGG application state is UNCONFIRMED (changelink applies it, but the later EBUSY
  leaves the overall op ambiguous).
- One unintended re-send occurred while fixing the tool's debug mode (idempotent, same
  EBUSY, no additional state change).

Per the strict gates: stopped. SET_EGRESS/SET_INGRESS NOT run. WDS not attempted.

---

# B1 EBUSY — root cause found (read-only investigation, 2026-11-06)

1. **Read-back (RTM_GETLINK, read-only, via rmnet-getlink):**
   rmnet_data1: ifindex=13, lower=12 (rmnet_ipa0), kind=rmnet, mux_id=1,
   **flags=0xC000000D (applied)**, ul_agg_params={**size=16000**, count=32, features=0,
   time=2987712}. The size 16000 = kernel normalization of 16384 in
   rmnet_map_update_ul_agg_config: get_order(16384)=2 → PAGE_SIZE<<2=16384, then
   minus SKB_DATA_ALIGN(sizeof skb_shared_info)=384 → 16000. So BOTH the flags and the
   UL agg params were applied by the changelink.

2. **agg_time validity (this exact kernel):** rmnet_rtnl_validate (the .validate op,
   runs before changelink): `if (agg_time < 1000000) return -EINVAL`. 2987712 ≈ 3 ms
   passes. (The value is the stock netmgrd default for dataformat_agg_ul_time, recovered
   from its config_load disasm — not from another branch.)

3. **Exact EBUSY origin: `dev_change_name()` (net/core/dev.c).** My message carried
   ifi_index=13 (>0) AND IFLA_IFNAME="rmnet_data1" → rtnl_newlink's existing-device path
   ran rmnet_changelink (applied flags+agg, rc=0) then fell through to do_setlink, which
   treats "index>0 + name present" as a RENAME → dev_change_name → `if (dev->flags &
   IFF_UP) return -EBUSY` (rmnet_data1 is UP) — the EBUSY check precedes the same-name
   early-return. Hence "some changes committed" (the changelink had already committed).
   NOT an rmnet/IPA fault. Fix: omit ifi_index (name-only changelink, like stock's
   rtrmnet_ctl_changevnd which sets no ifi_index).

4. **State consistency: CONSISTENT.** flags + UL agg fully applied; no IPA resources
   touched by the SETLINK; no corruption.

5. **B2 can proceed safely: YES.** handle3_egress_format reads only the ioctl's u.data
   flags (zero references to the rmnet core agg params) — SET_EGRESS does not depend on
   the UL agg params. The VND is already in the intended state.

---

# Option B — pipes UP, but WDS bind still Internal(3): the DPM gate (2026-11-06)

Executed: RTM SETLINK (flags 0xC000000D applied, UL agg {16000,32,0,2987712} applied,
normalized) → SET_EGRESS 0x0e rc=0 (TX pipe created) → SET_INGRESS 0x2e{16384,31} rc=0
(RX pipe + QMAP hdr + WAN rt created). Both pipes up (no fresh "pipe is not valid").

**WDS Bind Mux Data Port (mux 1, embedded, iface 1) → Internal (3) — UNCHANGED by the
pipes.** So the pipes were not the (only) gate.

ROOT CAUSE (recovered from stock netmgrd): netmgrd opens the embedded data port via the
**QMI DPM (Data Port Mapper) service 0x2F** before any WDS data-call operation
(netmgr_qmi_dpm_port_open; qmi_dpm_enabled=1 for msmnile). The modem's embedded data port
starts CLOSED; WDS bind on a closed port → Internal(3). We never sent DPM Open Port.
(This is also exactly the ModemManager qcom-soc gap noted in the earlier telephony doc:
MM's DPM open-port failed only because it couldn't read tx/rx_endpoint_id sysfs, which
this kernel doesn't expose — we HAVE the endpoint pair from rmnet-inspect: consumer=2,
producer=11.)

The fix path (next approved step): DPM Open Port (QMI DPM 0x2F, msg QMI_DPM_OPEN_PORT)
with the embedded endpoint {ctrl: embedded/1; hw-data: embedded, rx=2(consumer),
tx=11(producer)} THEN WDS bind mux 1. qmicli supports `--dpm-open-port` with all keys;
the exact stock arg values to be confirmed against netmgrd's call (or captured raw via
cap_qrtr.so on the first approved send).

State preserved: pipes up (harmless), VND configured, no WDS state change, modem untouched.

---

# Option B — FULL CONTROL-PLANE SUCCESS, data-path gap found (2026-11-06)

With DPM Open Port added before the WDS bind, the entire QMI control chain works on
Debian:
- DPM Open Port (QMI DPM 0x2F): SUCCESS — the embedded data port opens (the missing gate
  for the WDS bind; stock netmgrd does this at startup via netmgr_qmi_dpm_port_open).
- WDS Bind Mux Data Port (mux 1, embedded, iface 1): SUCCESS (result=0).
- WDS Start Network (IPv4): SUCCESS — packet_data_handle=0x05cb47d0, and the modem assigned
  **IPv4 10.159.216.78, gateway 10.159.216.77, MTU 1500** (MTS CG-NAT).

NOTE on CID lifecycle: a QMI client's CID dies with its process's QRTR socket on this
platform (unlike stock's persistent netmgrd), so bind+start must run in ONE process — the
wds-call helper (camera-recon/netmgr/wds-call.c) does exactly that and holds the call open.

## Data-path gap (the experiment's finding)

Call up + rmnet_data1 configured (10.159.216.78/32, host route + default via .77). But:
- UL packets enter the egress pipe (rmnet_data1 TX increments; rmnet_ipa0 TX forwards).
- The modem DID receive some (IPA wan_rx=7) — but rmnet_data1 RX=0, and no TCP/ICMP reply.
- IPA stats: wan_rx=7 (modem sent 7), but they never surfaced to the VND.

**Leading hypothesis (evidence-backed, not yet confirmed):** the port data_format
0xC000000D carries bit31 = RMNET_EGRESS_FORMAT_AGGREGATION → the rmnet CORE UL-aggregates
packets into multi-packet QMAP bundles (params {16000,32,0,2987712}), but the modem was
never told to UL-deaggregate (no WDA SET_DATA_FORMAT — it failed InvalidOperation). So the
modem drops the aggregated UL frames → carrier never sees the SYN → no DL reply.
This matches the "test the modem's plain-QMAP default" experiment's expected failure mode:
the stock flags assume the WDA-negotiated QMAPV4+agg session we could not establish.

**Proposed next experiment (needs approval):** clear bit31 (no rmnet-core UL agg) — port
flags 0x4000000D — via a name-only RTM SETLINK (no ifi_index → no rename EBUSY), keeping
the no-checksum ioctls; re-test data flow. Alternative: also re-examine the DL demux path.

State: call held by a background ssh (wds-call running, /tmp/wds-live.log on host).

---

# Data-path deep-dive (2026-11-06): UL works, DL dropped silently in the IPA RX path

After clearing rmnet-core UL aggregation (port flags 0x4000000D via a name-only
changelink — NO EBUSY this time; readback confirms flags=0x4000000d, agg params retained):

- UL works: TCP SYN to 8.8.8.8 enters the pipe, the modem replies (IPA wan_rx +2 per SYN).
- DL: the modem's reply frames reach the IPA (wan_rx increments) and are parsed by
  ipa3_wan_rx_pyld_hdlr WITHOUT any error (dmesg clean of IPAERR) — but they never reach
  rmnet_data1 (apps_ipa_packet_receive_notify never runs: rmnet_ipa0 rx_packets stays 0).
- So the silent drop is inside ipa3_wan_rx_pyld_hdlr's status-parse loop (a non-error
  skip path — split-buffer accumulation or an opcode it quietly skips), BEFORE the notify.
- Ruled out: FIX_ETHERNET bit0 (unused in this rmnet core); rmnet_ipa0 type=ARPHRD_RAWIP
  (the ingress ETHER branch is correctly skipped); mux/demux registration intact.
- Remaining suspect: the modem's actual default DL QMAP header format vs our hdr_len=4
  (no-csum) config — needs IPA-level debug tracing (IPADBG) to pin; IPADBG is compiled
  out without CONFIG_DYNAMIC_DEBUG.

This is the experiment's honest outcome: the modem's default DL framing is NOT yet
proven compatible with the no-WDA ingress config — the UL works, the DL parse drops frames.

---

# ═══════════ FINAL ROOT-CAUSE REPORT (2026-11-06) ═══════════

## THE root cause of the DL data-path drop — the QMI_IPA handshake is one-sided

**The modem never connects to the AP's QMI_IPA server**, so the modem→AP handshake
(INDICATION_REGISTER_REQ, INIT_MODEM_DRIVER_CMPLT_REQ, INSTALL_FILTER_RULE_REQ) never
happens → the AP's IPA never gets the Q6 filter rules → DL frames have no route to the
WAN consumer pipe → silently dropped before the rmnet core / netdev.

Evidence (debug image 305, D2QQMI/D2QRX traces):
- D2QQMI: zero modem→AP QMI_IPA messages all boot (only AP→modem INIT_MODEM_DRIVER_REQ
  + its RESP completed — that's the AP's CLIENT, which works because the AP initiates it).
- D2QRX (handler): never invoked for DL — the drop is upstream of the handler.
- The AP's QMI_IPA server (svc 0x31, inst 0x101) is NS-announced at **node 1 port 0x0
  (invalid)** — AND ns-lite's modem-push table is STATIC={0x0E rmtfs, 0x00 CTL, 0x40}
  with NO 0x31, so the kernel's QMI_IPA service is never relayed to the modem.

Why the modem can't connect:
- The kernel creates the QMI_IPA server socket with sock_create_kern + getsockname but
  **never binds it** (no kernel_bind anywhere in qmi_interface.c / ipa_qmi_service.c) →
  the announcement carries port 0.
- AND ns-lite doesn't forward the kernel's service announcements to the modem (it only
  pushes its STATIC table). On stock the kernel's own qrtr-ns relays it.

So on Debian the modem literally cannot address the AP's QMI_IPA server. UL works because
the AP→modem INIT is a client→server push (no AP-server reachability needed).

## The fix (next session)

Make the AP's QMI_IPA server reachable + announced to the modem:
1. Give the kernel's QMI_IPA server a real bound QRTR port (or confirm the ephemeral one
   and announce THAT), and
2. add svc 0x31 to ns-lite's modem-push set at that port (same mechanism as rmtfs/CTL).
Then the modem connects, sends INDICATION_REGISTER + INSTALL_FLTR_RULE, the IPA gets the
DL filters, and DL flows.

Open implementation detail to nail first: read the kernel's QMI_IPA server socket's actual
bound QRTR port at runtime (the announced port-0 may be a stale getsockname of an unbound
socket; the real ephemeral port may differ). Then announce that port.

## Full state of the work

CONTROL PLANE (FULLY WORKING on Debian, image 303/304/305):
- rmnet_data1 (mux 1) on rmnet_ipa0; port data_format 0x4000000D (QMAP, no UL-core-agg);
  UL agg params {16000,32,0,2987712}; ADD_MUX_CHANNEL(1, rmnet_data1); SET_EGRESS 0x0e;
  SET_INGRESS 0x2e{16384,31} (both pipes up); DPM open port (the gate for the bind);
  WDS bind mux 1; WDS start IPv4 → real carrier IP assigned.

Data path: UL fully works (modem receives + replies); DL drops in the IPA (no route) —
root cause above.

## Artifacts inventory

Host (/home/usb/mnt-sda/camera-recon/netmgr/):
- Tools (also on phone /root/): rmnet-setup.c (0x89FD ioctls), rmnet-rtm.c (RTM
  flags/agg, name-only to avoid the rename EBUSY), rmnet-getlink.c (read-back),
  wda-setdf.c + wda_raw.py (WDA), wds-call.c (persistent bind+start+hold),
  cap_qrtr.c (LD_PRELOAD raw QRTR capture), d2q-data-up.sh (full bring-up).
- Evidence: wda-raw-final.log (raw WDA txns), wds-session-pre-304.log,
  session-pre-304.log, netmgrd.dis + decomp dumps, cci_decomp.txt, libqmi_cci.dis,
  libqmiservices.dis, rmnetctl64_decomp.txt, rtrmnet64_decomp.txt.
- Ghidra project: ghidra-proj/netmgrJ (netmgrd, librmnetctl64, libqmi_cci).

Kernel instrumentation (ShinyKernel-gpu-exp tree, ipa_v3):
- ipa_dp.c: D2QRX tracepoints (wan_rx handler entry/hexdump/status/branches/deliver/exit,
  handle_skb_completion EOB dispatch) + d2q_hexdump helper + d2q_rx_trace gate.
- ipa_qmi_service.c: D2QQMI tracepoints (INDICATION_REGISTER / INSTALL_FILTER_RULE /
  INIT_CMPLT / MASTER_DRIVER_INIT_COMPLETE send).
- config: d2q_debug.config = d2q_a6xx_drm_experimental.config + CONFIG_DYNAMIC_DEBUG=y.
- Build: .opencode/build-debug.sh (FRAGMENT=d2q_debug.config).

Images: 303 = current stable (recovery); 304 = debug (handler-level); 305 = debug
(+ QMI_IPA + EOB tracepoints) — currently booted.

## The complete recovered stock init sequence (for the record)

netmgrd (msmnile) at boot/data-path-up, in order:
1. /dev/ipa trigger (early-boot) → IPA fw + QMI_IPA init + rmnet_ipa0 created.
2. RTM create rmnet_data<i> (mux = i+1) via rtrmnet_ctl_newvnd.
3. ADD_MUX_CHANNEL(0x89FD/5) per link: phys=rmnet_ipa0, mux=i+1, name=rmnet_data<i>.
4. netmgr_qmi_dpm_port_open: DPM Open Port (QMI DPM 0x2F) — the data-port-open gate.
5. netmgr_qmi_set_data_format → WDA SET_DATA_FORMAT (QMAPV4 both dirs, dl 16384/31,
   ul 16384/32, fc=1, dl_pad=0, ll=raw-ip) then netmgr_kif_set_data_format ioctls:
   SET_EGRESS u.data=0x1e, SET_INGRESS u.data=0x3e {16384,31} + RTM changevnd port flags
   0xC000000D (mask=flags) + UL agg RTM {16384,32,0,2987712}.
6. Data call: WDS bind mux (i+1) embedded iface 1 → WDS start network.

WDA note: WDA SET_DATA_FORMAT returns InvalidOperation(70) over the QRTR WDA instance
even byte-exact (the QRTR WDA has no data-port context; the real one is the CCI/QRTR
session with the full netmgrd context). It is gated BEFORE the ioctls on stock
(netmgr_qmi_set_data_format returns early on WDA failure) — so on stock WDA succeeds;
we could not reproduce the WDA success, hence the no-checksum (0x0e/0x2e) variant.

## Safety record

No driver unbind/rebind, no modem restart, no module reload, no repeated /dev/ipa
trigger. All three debug builds used separate image files; 303 = recovery. One
idempotent RTM SETLINK re-send during tool debugging (no new state). The IPA pipe
allocation (SET_EGRESS/INGRESS) is per-boot and persistent-until-reboot by design.
