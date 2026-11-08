# QMI_IPA service registration on d2q — root cause + fix (2026-11-06)

**Device:** Samsung Galaxy Note10+ SM-N9750 (d2q, SM8150), Debian, ShinyKernel 4.14.190
(image 305, kernel #123 — debug image with D2QQMI/D2QRX tracepoints).
**Companion docs:** TELEPHONY-DATA-PATH-2026-11-05.md, RMNET-NETMGR-STOCK-ANALYSIS-2026-11-06.md.

## TL;DR

The modem never contacted the AP's QMI_IPA server because **the service was never
announced to the modem at a reachable address**:

1. The kernel QMI layer registers the QMI_IPA server (svc `0x31`, v1, inst 1 →
   combined instance `0x101`) with a NEW_SERVER control packet whose **payload says
   `node 1, port 0`** — a stale address cached before the QRTR socket's autobind.
   The socket itself **does** get a real ephemeral port (autobind on first send works
   on this kernel — Case 3 of the task brief is **ruled out**).
2. On this 4.14 CAF fork there is **no in-kernel QRTR name service**; the ctrl port
   owner is the userspace NS = `ns-lite`. The kernel **never** forwards local service
   registrations to remote nodes (`qrtr_sendmsg` → local delivery only). So the
   modem can only learn AP services if ns-lite pushes them.
3. ns-lite (a) cached the **payload** `(1, 0)` — never the real source port — and
   (b) only ever pushed its STATIC table `{0x0E rmtfs, 0x00 CTL, 0x40 servreg}` to
   the modem — **0x31 was never announced**. The modem never sends NEW_LOOKUP to the
   AP (its NS log shows only HELLO/DEL_CLIENT from node 0), so push is the only
   discovery channel.

Result before the fix: zero modem→AP QMI_IPA messages all boot (D2QQMI count = 0),
no INDICATION_REGISTER, no INIT_CMPLT, no INSTALL_FLTR_RULE → the IPA never received
the Q6 filter rules → DL unrouted.

**Fix (userspace only, no kernel change):** ns-lite v3 — for LOCAL senders
(node 1) cache the **source address** of the NEW_SERVER control packet (the real
bound port), not the payload; announce every learned local service to the modem
(immediately + every 5 s, same mechanism as the STATIC push); handle
DEL_SERVER/DEL_CLIENT/BYE for teardown and modem restart. Plus a systemd ordering
fix (`d2q-ipa-fw.service: After=d2q-qrtr-ns-lite.service`) so the kernel's
registration never races the NS startup.

**Verified live after reboot:** AP QMI_IPA server announced at `node 1 port 0x4043`;
the modem sent `INDICATION_REGISTER_REQ` (t=11.751 s), `INIT_MODEM_DRIVER_CMPLT_REQ`
(11.758), `INSTALL_FILTER_RULE_REQ` with **13 rules** (12.750); the rules installed
on `IPA_CLIENT_APPS_WAN_PROD` at SET_EGRESS; the `ipa3_qmi_filter_request_send`
state gates (`indication_fin && modem_init_fin && ipa_q6_clnt`) are now all open.

---

## 1. Proven facts vs hypotheses

### Proven (live evidence this boot)

| # | Fact | Evidence |
|---|------|------|
| F1 | Kernel `qmi_sock_create()` caches `{node=1, port=0}`: `sock_create_kern` + immediate `kernel_getsockname`, no bind | `qmi_interface.c:604-628`; `qrtr_create` sets `us.sq_port=0`, SOCK_ZAPPED (`qrtr.c:2021-2029`); `qrtr_getname` returns `ipc->us` verbatim |
| F2 | The NEW_SERVER payload for QMI_IPA says `node=1 port=0` | ns-probe lookup answer (before fix): `svc=0x31 inst=0x101 -> node 1 port 0x0`; ns-lite v3 log: `local svc 0x31 inst 0x101 -> 1:0x4043 (payload said 1:0x0)` |
| F3 | The QRTR socket **does** get a real ephemeral port at first send (autobind) | `qrtr_sendmsg → qrtr_autobind` (`qrtr.c:1631`, port range `0x4000+`); empirically the kernel QMI **client** socket's lookup came from port `0x4044` this boot; the server's real port = `0x4043` |
| F4 | The registration is delivered only **locally** to the ctrl-port owner (ns-lite); the kernel never relays it to the modem | `qrtr_sendmsg`: `addr->sq_node == ipc->us.sq_node` → `enqueue_fn = qrtr_local_enqueue` (`qrtr.c:1652-1653`); `qrtr_fwd_ctrl_pkt` only forwards packets **received from** remote nodes (`qrtr.c:1062`) |
| F5 | The delivered packet's **source address** = the real port; the payload = port 0 | `qrtr_local_enqueue` stamps `cb->src = from = &ipc->us` (`qrtr.c:1557-1559`); ns-lite v3 saw source `1:0x4043` vs payload `1:0x0` |
| F6 | The modem never sends NEW_LOOKUP to the AP | ns-lite log: from node 0 only `cmd 2 (HELLO)` and `cmd 6 (DEL_CLIENT)` — zero `cmd 10 (NEW_LOOKUP)` |
| F7 | ns-lite (pre-fix) cached the payload and never pushed 0x31 | v2 code: `cache[(svc,inst)] = (f[2], f[3])` (payload); push loop iterates STATIC only |
| F8 | There is no in-kernel name service on this fork | `net/qrtr/` has no `ns.c`; `qrtr_port_lookup(QRTR_PORT_CTRL)` → userspace socket (ns-lite). `qrtr_local_enqueue` **drops** packets to the ctrl port when no userspace socket owns it (`qrtr.c:1538-1541`) |

### Hypotheses rejected

- **H: "The socket stays at port 0 / kernel never assigns a port" (Case 3).**
  Rejected by F3 — autobind works; the client socket visibly got `0x4044`.
- **H: "The boot-time registration is lost because ns-lite binds after the IPA QMI init."**
  Rejected for the observed boots: the registration **was** cached (port 0) pre-fix,
  i.e. it arrived after ns-lite bound (ns-lite binds ~5.17 s; the QMI service-init
  worker runs on a workqueue after the fw load — later than the `post_init` log).
  The `After=` ordering fix was still added as insurance against the race.
- **H: "Fixing the kernel payload (explicit bind in qmi_sock_create) is required."**
  Not required: the NS is expected to use the source address for local registrations
  (this is exactly what upstream `net/qrtr/ns.c` does). The correct layer to fix is
  the NS (ns-lite), per the task's Case 2.

## 2. The exact registration sequence (after the fix)

Tuples are `(node, port)`; all on QRTR.

```
AP kernel ipa3_svc_handle           AP ns-lite (1, 0xfffffffe)          Modem NS (0, 0xfffffffe)
        |                                     |                                |
        | NEW_SERVER{svc=0x31,inst=0x101,     |                                |
        |   payload node=1, port=0}           |                                |
        |  -- src addr = (1, 0x4043)  ------> |  (local delivery; v3 caches    |
        |                                     |   (0x31,0x101) -> (1,0x4043))  |
        |                                     | NEW_SERVER{0x31,0x101,1,0x4043}|
        |                                     |  -------------------------->   |  (+ every 5 s)
        |                                     |                                | modem NS registers
        |                                     |                                | AP QMI_IPA @ (1,0x4043)
        |  <============ modem IPA client connects to (1, 0x4043) ============ |
        |  RX QMI_IPA_INDICATION_REGISTER_REQ_V01   (t=11.751)                 |
        |  TX INDICATION_REGISTER_RESP                                       |
        |  (after AP INIT_MODEM_DRIVER completes) TX MASTER_DRIVER_INIT_       |
        |      COMPLETE_IND to the modem's client port                         |
        |  RX QMI_IPA_INIT_MODEM_DRIVER_CMPLT_REQ_V01 (t=11.758)               |
        |  RX QMI_IPA_INSTALL_FILTER_RULE_REQ_V01 — 13 UL rules (t=12.750)     |
```

The parallel client direction (unchanged, already worked): AP `ipa_q6_clnt`
looks up the modem's server `0x31/0x201` → ns-lite caches the modem's announcement
`(0x31,0x201) -> (0, 0x2a)` and pushes it to the kernel client →
`kernel_connect` → `INIT_MODEM_DRIVER_REQ` (t=11.737) → RESP (11.758).

## 3. The code responsible for the failure

Two compounding defects, both in userspace ns-lite (kernel behavior is quirky but
within QRTR semantics):

**Defect A — payload-trusting cache (`/usr/local/bin/qrtr-ns-lite.py` v2):**
```python
if cmd == CMD_NEW_SERVER:
    cache[(f[0], f[1])] = (f[2], f[3])   # <-- payload node/port; (1,0) for local
```
For local registrations the payload is stale (F2); the real endpoint is the
recvfrom source address.

**Defect B — no modem-facing relay of local services:** the periodic announce loop
iterated only `STATIC = {0x0E, 0x00, 0x40}`; learned local services (0x31, memshare
0x34, …) were never announced to the modem, and the modem never asks (F6).

(Underlying kernel quirk, not a bug per se: `qmi_send_new_server` serializes the
pre-autobind cached `qmi->sq` — `qmi_interface.c:252-253`. Left as-is: the NS must
not trust it, and with v3 it doesn't matter.)

## 4. The patch (unified diff)

`ns-lite v2 → v3` (full file: `camera-recon/qmi-ipa-fix/qrtr-ns-lite-v3.py`,
deployed as `/usr/local/bin/qrtr-ns-lite.py`; v2 backed up as
`/usr/local/bin/qrtr-ns-lite.py.v2`):

See `camera-recon/qmi-ipa-fix/ns-lite-v2-to-v3.diff`. Summary:

1. NEW_SERVER from a **local** sender (`addr[0] == 1`): cache `(svc,inst) ->
   addr` (the real source port); record in `local_svcs`; log payload-vs-source;
   immediately announce to the modem.
2. NEW_SERVER from a **remote** sender: unchanged (payload is authoritative).
3. Periodic announce loop: STATIC **plus** every `local_svcs` entry (with the
   learned instance field, e.g. `0x101`).
4. NEW: DEL_SERVER → drop + de-announce to modem; DEL_CLIENT → drop any service
   at the dead `(node,port)` + de-announce (the QMI layer on this fork never emits
   DEL_SERVER — socket teardown only broadcasts DEL_CLIENT); BYE from node 0 →
   drop modem-side cache (modem restart); local services survive and are
   re-announced by the periodic loop.
5. Lookup answers and lookup-client pushes now use the corrected address.

`d2q-ipa-fw.service` (ordering insurance):
```diff
 [Unit]
 Description=Load the IPA firmware (creates rmnet_ipa0 for the modem data path)
+# The IPA QMI server (svc 0x31) registers with ns-lite at the end of the
+# firmware load; ns-lite must already own the QRTR ctrl port or the kernel
+# registration is dropped (qrtr_local_enqueue kfree with no ctrl listener).
+After=d2q-qrtr-ns-lite.service
+
 [Service]
```

## 5. Why this is correct for this kernel (4.14 CAF fork)

- QRTR name service = the ctrl-port owner. This fork has no in-kernel NS; the
  kernel's only jobs are (a) deliver ctrl packets addressed to the local node to
  the ctrl-port owner with the true source address, (b) forward remote-originated
  ctrl packets between nodes. Both work. The missing modem-facing relay for local
  services is definitionally the userspace NS's role (this is what qrtr-ns does on
  stock; what upstream's in-kernel `ns.c` does later).
- Using the sender's `sockaddr_qrtr` for local registrations mirrors upstream
  `net/qrtr/ns.c` semantics (it registers the service at the sender's address,
  ignoring a stale payload). The kernel QMI payload quirk (`qmi->sq` cached
  pre-autobind) becomes irrelevant.
- No hardcoded ports: the ephemeral port is learned per boot (it moved between
  boots: `0x4043` this boot). Handles re-registration (ENETRESET →
  `qmi_handle_net_reset` re-sends NEW_SERVER for all services — v3 re-caches and
  re-announces the new port immediately) and modem restart (BYE handling +
  5 s re-announce loop).
- No duplicate advertisements: the kernel produces none toward the modem; v3 is
  the only announcer.

## 6. Test evidence (before → after)

**Before (image 305, pre-fix boot):**
- ns-probe lookup `0x31/0x101` → `node 1 port 0x0` (invalid).
- `journalctl -k | grep -c D2QQMI` = **0** — zero modem→AP QMI_IPA messages.
- IPA ipc log: zero `install_flt` lines.

**After (same image, ns-lite v3 + ordering fix, reboot):**
- ns-lite: `local svc 0x31 inst 0x101 -> 1:0x4043 (payload said 1:0x0)`,
  `announced local svc 0x31 to modem`.
- ns-probe lookup `0x31/0x101` → `node 1 port 0x4043`; `0x31/any` → both the AP
  server `(1, 0x4043)` and the modem server `(0, 0x2a)`.
- D2QQMI: `RX INDICATION_REGISTER_REQ` (11.751), `RX INIT_MODEM_DRIVER_CMPLT_REQ`
  (11.758), `RX INSTALL_FILTER_RULE_REQ` (12.750).
- IPA ipc log: `ipa3_copy_ul_filter_rule_to_ipa: Received (13) install_flt_req_ex_list`.
- Control plane re-run (`d2q-data-up.sh`): ADD_MUX rc=0 (now with rules present →
  full `register_to_ipa` path), SET_EGRESS 0x0e rc=0 (UL rules installed →
  FILTER_INSTALLED_NOTIF to modem), SET_INGRESS 0x2e rc=0, DPM open SUCCESS,
  WDS bind SUCCESS, WDS start SUCCESS → IPv4 10.29.252.141/32, gw 10.29.252.142.

## 7. Remaining gap (exposed by this fix): the downlink data path

Criteria 1–6 are met (the assigned registration fix). Criteria 7–8 (DL traffic)
are **not** yet. The DL investigation found two real bugs and one record
correction, then hit the limit of AP-side-only analysis:

### 7a. Record correction — `wan_rx` was misread in the prior session

The 2026-11-05/06 report's "the modem replies (wan_rx +2 per SYN)" was a misread.
On this kernel the IPA debugfs `stats` block is largely **non-functional**
(`sw_tx`/`hw_tx`/`tx_compl` never move; `wan_rx` has no increment site in
`ipa_v3/ipa_dp.c`). The counter that *does* track UL activity is
`lan_rx_excp[IPAHAL_PKT_STATUS_EXCEPTION_NONE]` — but those are **IPA status/TAG
frames** on the LAN pipe (`lan_rx_pyld_hdlr`: "STATUS opcode=0 src=5 dst=10"),
not modem DL data. **DL has never been observed working on Debian** (pre- or
post-fix). The authoritative DL signal is the D2QRX tracepoint in
`ipa3_wan_rx_pyld_hdlr` (always-on, pr_err→dmesg): it has fired **0 times**.

### 7b. Real bug found + fixed — rmnet_ipa0 was DOWN

`d2q-data-up.sh` never set the **lower** device UP (it only existence-checked
`rmnet_ipa0` and set `rmnet_data1` up). With `rmnet_ipa0` DOWN the rmnet core
could not forward UL to it — `ipa3_wwan_xmit` was never called (tx_packets AND
tx_dropped both 0). **Fixed in the script (both copies): `ip link set
rmnet_ipa0 up`.** After this, UL genuinely reaches the IPA producer pipe
(`rmnet_ipa0` tx_packets increments, `ipa3_tx_dp(APPS_WAN_PROD)` succeeds).

### 7c. WDA SET_DATA_FORMAT now SUCCEEDS (was InvalidOperation)

Post-fix, `wda_raw.py` returns `result=0 error=0` (it failed `InvalidOperation(70)`
byte-exact before). The completed QMI_IPA handshake was the gate. The granted
config = plain QMAP, **no aggregation** (proto TLVs 0x12/0x13 = 0), so the matching
pipe config is the no-agg variant (egress `0x0a`, ingress `0x0e`).

### 7d. DL filter rules to the modem are accepted but ineffective (so far)

Built `dl-rule` (`qmi-ipa-fix/dl-rule.c`): `/dev/wwan_ioctl` `WAN_IOC_ADD_FLT_RULE_EX`
→ `ipa3_qmi_filter_request_ex_send` → `QMI_IPA_INSTALL_FILTER_RULE_EX_REQ` to the
modem — all state gates open now. The modem **accepts** the rule (rc=0) for a
dst-IP match and for match-all, with action ROUTING (and EXCEPTION variant).
**No DL flows regardless.** Concrete suspect found: the rule references
`route_table_index = 9` (the AP's `ipa_dflt_wan_rt`), but the modem's allocated
v4 routing-table range is **[0,7]** (`v4_modem_rt_index_hi = 0x7`,
`ipa_utils.c:3205`) — an out-of-range index the modem may silently not program.
What stock actually references needs pinning from a **stock-boot capture** of the
working data call (the modem-side view is not reachable from the AP).

### Current DL status (honest)

UL enters the IPA and transmits cleanly; the modem never delivers DL frames to
the WAN consumer (D2QRX=0; even the call's own gateway, the modem itself, times
out). Every AP-side prerequisite is now in place (handshake, WDA, DPM, pipes,
DL rule). The remaining blocker is **modem-side data-path activation** — what
makes the modem's IPA forward DL to the AP on stock. Efficient next step: a
stock-boot capture of the QMI_IPA exchange + DL data path during a working call
(stock backup exists: `stock/BACKUP-2026-11-03.md`), to recover the exact DL
rule/route the modem honors.

### Test tools (host `camera-recon/qmi-ipa-fix/`, phone `/root/`)

- `ns-probe.py` — query ns-lite's cache for a service (the port-0 vs real-port
  probe used for the before/after evidence).
- `dl-rule.c` (+ `linux/` UAPI headers) — install a modem DL filter rule.
- `d2q-stock-seq.sh` — full stock-ordered bring-up + test.
- `recall.sh` — restart the data call without the pkill self-kill trap.
- `decomp_ipacm.py` + `ipacm-decomp.txt` + `ipacm.dis` — the ipacm RE artifacts.

## 8. DL follow-up investigation (2026-11-06, second round) — hypothesis results

Following the task's hypothesis-driven plan (no flash; RE + controlled experiments):

- **Hypothesis A (route_table_index mismatch) — REJECTED.** The stock ipacm
  `IPACM_Config::InitEv` sets config+0x724 = `"ipa_dflt_wan_rt"`, and
  `install_wan_filtering_rule` queries exactly that name for the modem-bound DL
  rule (`QUERY_RT_TBL_INDEX`). So stock references the **same** AP table (idx 9)
  we used. The [0,7]-vs-9 range split is allocation bookkeeping, not
  hardware-enforced — an AP-range index is what stock sends too. Index 9 is NOT
  the bug.
- **Hypothesis B (route contents) — verified correct.** Live table dump
  (`cat /sys/kernel/debug/ipa/ip4_rt` → dmesg): `tbl_idx 9 ipa_dflt_wan_rt:
  rule dst=35 ep=11` = `IPA_CLIENT_APPS_WAN_CONS`. Correct DL-consumer target.
  (Also confirms ep_mapping: APPS_WAN_CONS = ep 11 = DL, APPS_WAN_PROD = ep 2 = UL.)
- **Hypothesis C (filter matching) — rule content matches stock** (dst-IP match +
  the same table). Both dst-IP and match-all variants tested; no DL either way.
- **Hypothesis D (endpoint config / DPM rx-tx swap) — TESTED, no effect.** Found
  that `GET_EP_PAIR` returns swapped field labels (`producer_pipe_num` is filled
  with the APPS_WAN_**CONS** endpoint, `consumer_pipe_num` with APPS_WAN_**PROD**),
  so the prior session's `rx=2/tx=11` was derived from mislabeled output. Clean-boot
  test with the corrected `rx=11/tx=2`: **no change** — DPM endpoint values do not
  move the physical path (the GSI channels are direction-fixed).
- **Hypothesis F (QMAP framing/aggregation) — TESTED, no effect.** Both agg
  (egress 0x0e/ingress 0x2e) and no-agg (0x0a/0x0e) pipe configs fail identically.
  WDA SET_DATA_FORMAT now **succeeds** post-registration-fix and grants QMAPV4.
- **Hypothesis E (missing activation) — OPEN, needs stock ground truth.** The
  modem's own IPA stats query (QMI_IPA GET_DATA_STATS, msg 0x0030, to the modem's
  svc 0x31/inst-2 server) returns SUCCESS with **empty** pipe and filter lists —
  i.e. the modem reports no UL seen and no DL forwarded (or stats not enabled —
  indistinguishable AP-side). UL verifiably enters the AP IPA
  (rmnet_ipa0 tx increments; `ipa3_tx_dp(APPS_WAN_PROD)` succeeds); nothing ever
  reaches the WAN consumer handler (D2QRX=0). Even the call's own gateway (the
  modem itself) is unreachable.

**Conclusion (pre-stock):** every AP-side prerequisite is in place and the config
is stock-faithful; the modem's IPA never forwards DL to the AP.

## 9. STOCK-BOOT CAPTURE (2026-11-06 evening) — ground truth

Phone flashed to stock Android (boot image only; Debian rootfs at
`/userdata/rootfs.img` survives), adb root via KernelSU. Cellular data call up on
`rmnet_data0` (mux 1), IP 10.167.223.172/29 (MTS). **DL CONFIRMED WORKING on
stock:** `ping -c3 -I rmnet_data0 8.8.8.8` → 3/3, 0% loss.

### What stock actually does (kernel-level, from the IPA ipc logbuf)

Data-path bring-up sequence (from `/sys/kernel/debug/ipc_logging/ipa/log`):
1. `WAN_IOC_ADD_FLT_RULE_EX` with **0 rules** to Q6 (a modem-DL-rule CLEAR).
2. ipacm route churn on its own table **10 = `COMRTBLLANv4`** (del+add 3 routes,
   all `dst=35 ep=11` = APPS_WAN_CONS).
3. `QUERY_RT_TBL_INDEX` (nr=34) then `GENERATE_FLT_EQ` **×2**, then
   `WAN_IOC_ADD_FLT_RULE_EX` with **2 rules** to Q6 (1768-byte request).
4. Modem → AP: `FILTER_INSTALLED_NOTIF` (the modem confirms it installed the
   AP's DL rules) — `ipa3_handle_filter_installed_notify_req` result 0.

### Stock vs Debian

| Parameter | Stock | Debian |
|---|---|---|
| DL rules sent to modem | **2** | 1 (my dl-rule) |
| Rule clear-first (0-rule call) | yes | no |
| AP-side ipacm WAN table | `COMRTBLLANv4` (tbl 10, 3 routes→ep 11) | absent (only kernel dflt_wan_rt tbl 9→ep 11) |
| FILTER_INSTALLED_NOTIF modem→AP | received | **unknown — must check** |
| DL forwarding | **works** | none |

### What this rules in/out

- The route target (ep 11 / APPS_WAN_CONS) is identical on both — NOT the bug.
- The DL rule construction (dst-IP match, ROUTING, rt_tbl from QUERY) matches the
  ipacm RE — my dl-rule is content-correct in structure.
- **The concrete stock deltas to replicate:** (a) the modem-DL-rule CLEAR before
  install; (b) **2 rules** not 1; (c) confirm the modem actually installs them
  (the FILTER_INSTALLED_NOTIF round-trip) — if Debian never gets the notif, the
  modem silently rejected the rule.
- ipacm logs go to `/dev/socket/ipacm_log_file` → ipacm-diag → DIAG (not logcat);
  the exact rule bytes need a DIAG capture or the RE completion. The stock kernel
  has kprobes SELinux-blocked and no strace, so the exact eq-attrib bytes were not
  captured this session.

## 10. The decisive reframe (2026-11-06 late, on Debian) — it's UL, not DL

Back on Debian with dynamic debug on the IPA QMI path, the decisive finding:
**the modem DID install my DL rule — `ipa3_handle_filter_installed_notify_req`
fires (result 0, err 0) after `dl-rule`.** The DL rule is NOT rejected.

But data still doesn't flow. The decisive discriminator — a modem-side bearer
packet-statistics query (added to a persistent `wds-call2`, WDS
GET_PKT_STATISTICS on the live call): **`rx_ok=0 tx_ok=0`** even while I send UL.
**The modem's bearer NEVER receives our UL packets.** UL framing captured raw on
`rmnet_ipa0` (AF_PACKET) is **correct**: `00 01 00 2e …` QMAP header (mux 1) + a
valid IPv4 UDP DNS frame `10.167.223.172 → 8.8.8.8:53`.

So the whole DL-rules framing was a red herring for the current symptom: the
modem's IPA receives our well-formed UL packets (GSI TX completes) but drops them
before the bearer — the modem's bearer↔IPA link is broken. The DL replies never
happen because UL never reaches the network. (This also re-frames the previous
session's "wan_rx +2/SYN" — there was never real DL; those were IPA status/TAG
frames on the LAN pipe.)

Tested and REJECTED as the cause: rt index (A), route contents (B), filter match
(C), DPM rx/tx endpoint swap (D), WDA session persistence, QMAP aggregation vs
no-agg, QMAP checksum offload vs not, WDA timing. All AP-side config is now
stock-faithful and the modem installs the rules.

**Remaining unknown (modem-side):** why the modem's IPA doesn't route our UL to
the bearer. Needs the FULL stock QMI exchange (all services, not just QMI_IPA)
or the modem's DIAG to pin down. The `wds-call2` tool (with the stats query) is
at `/root/wds-call2` (source `camera-recon/netmgr/wds-call.c`).

## 11. STOCK vs DEBIAN — the missing activation (2026-11-07)

Second stock boot captured via the kernel **qrtr_0 ipc log**
(`/sys/kernel/debug/ipc_logging/qrtr_0/log`, present + unconditional on both the
stock and ShinyKernel). Decoding the full AP<->modem exchange over the QMI_IPA
service (modem port 0x4b on stock) shows a message Debian **never** sends:

**`QMI_IPA_ENDP_DESC_INDICATION` (msg `0x003f`) — sent 7x at boot by `rild`
(`ESAR` = /vendor/bin/hw/rild), the Samsung RIL daemon.** Payload
`ipa_endp_desc_indication_msg_v01` carries `ipa_ep_id_type_v01[]` =
`{ic_type, ep_type, ep_id, ep_status}`. The kernel's own reference implementation
of this message (`ipa_send_rsc_pipe_ind_to_modem`, rmnet_ipa.c:1540) sends exactly:
`ep_id = ipa3_to_apps_hdl` (the AP's **WAN consumer** pipe), `ic_type = AP`,
`ep_type = RSC_PROD`, `ep_status = CONNECTED`.

This is the **pipe-endpoint handshake**: it tells the modem that the AP's WAN
consumer endpoint exists and is CONNECTED — i.e. *where to deliver downlink*, and
by symmetry that the AP's pipes are up. Without it the modem never wires its
bearer to the AP's IPA pipes, so **no packet crosses in either direction** — which
is exactly the measured failure (`wds-call2` modem bearer `rx_ok=0 tx_ok=0`, GSI
TX completes, D2QRX never fires).

**Why Debian lacks it:** the kernel only sends this via
`ipa_send_rsc_pipe_ind_to_modem` gated on `dev->features & NETIF_F_GRO_HW`
(rmnet_ipa.c:1669, in `handle3_ingress_format`). The driver *advertises* GRO_HW in
`hw_features` (rmnet_ipa.c:2882) but the gate reads the *active* `features`. On
stock it is moot because **rild sends `0x3f` directly** (userspace). Debian has no
rild and (unverified) possibly no active GRO_HW -> the indication is never sent.

**Fix candidates (to test on Debian):**
1. Enable `NETIF_F_GRO_HW` on rmnet_ipa0 so the kernel's
   `ipa_send_rsc_pipe_ind_to_modem` fires during the ingress ioctl, or
2. send the `QMI_IPA_ENDP_DESC_INDICATION` to the modem explicitly (the kernel
   exposes `ipa3_qmi_send_endp_desc_indication`), or
3. confirm rild's exact 7 payloads and replicate them.
Decisive test: after flashing back to Debian, grep qrtr_0 for `0x3f`; if absent,
apply (1)/(2) and re-test real UL/DL traffic.

Capture: `camera-recon/qmi-ipa-fix/stock-boot-qrtr0.log` (boot exchange),
`decode_qrtr.py` (log decoder).

## 12. D2QFIX DEPLOYED AND TESTED (2026-11-07, image 306) — kernel patch result

**Fix candidate (1) was implemented, deployed, and TESTED. Result: the kernel
sends the endpoint descriptor, but the data path still doesn't flow.**

### The patch

`rmnet_ipa.c:1669` — removed the `dev->features & NETIF_F_GRO_HW` gate:

```diff
-	if (dev->features & NETIF_F_GRO_HW)
-		ipa_send_rsc_pipe_ind_to_modem();
+	/* D2QFIX: unconditional — the GRO_HW gate is never satisfied */
+	pr_err("D2QFIX: sending RSC pipe ind (GRO_HW gate removed)\n");
+	ipa_send_rsc_pipe_ind_to_modem();
```

Built as kernel #124, packed as image **306** (`artifacts/flash/306-halium-boot-d2qfix.img`,
SHA `1ca354248cfbeeaa…`), same ramdisk+cmdline as 305 (pack-d2q.sh with
RAMDISK=/tmp/rd305.bin extracted from 305, REFERENCE=305, header check passed —
"the ONLY functional difference from the reference is the kernel").

### Verified working (the patch itself)

- **D2QFIX marker fires** in journal at every ingress ioctl run (e.g. `Nov 07
  12:44:01 D2QFIX: sending RSC pipe ind (GRO_HW gate removed)`).
- **`0x3f` appears in qrtr_0** (count=1 per ingress run) — the kernel DOES
  send `QMI_IPA_ENDP_DESC_INDICATION` to the modem.
- The GRO_HW gate was confirmed as never satisfiable: `ethtool -k rmnet_ipa0`
  shows `rx-gro-hw: off [fixed]` — cannot be enabled at runtime.

### But the data path still doesn't flow

Even with D2QFIX firing and the WDS call up (verified: bind SUCCESS, start
SUCCESS, IP assigned), the modem bearer reports **`rx_ok=0 tx_ok=0`** and
D2QRX (WAN consumer handler) never fires. **The kernel's indication path is
not sufficient.**

### Why: the kernel path ≠ the rild path

The kernel sends the 0x3f via `qmi_send_indication(ipa3_svc_handle, &client_sq,
...)` — an **indication from the AP's server to the modem's client**. But
stock's `rild` sends 0x3f as a **REQUEST (type 0) from userspace to the
modem's SERVER port**. These are completely different message flows:

| | Kernel (D2QFIX) | Stock rild |
|---|---|---|
| QMI type | 4 (indication) | 0 (request) |
| From | AP's QMI_IPA server | rild's QMI client |
| To | modem's client port | modem's server port |
| Modem action | (none observed) | processes + responds |

Userspace attempts to replicate rild's request path were **rejected by the
modem**: type-0 request → error 57 ("invalid message ID"), type-4 indication
to server → error 1 ("malformed"). The modem only accepts 0x3f from rild's
specific QMI client context (likely a properly connected qmi_client instance,
not an ad-hoc QRTR socket).

### Additional finding: WDS bind intermittently fails

On fresh boots after multiple reboots, the WDS `bind_mux_data_port`
intermittently times out ("Transaction timed out"). The call eventually comes
up with the persistent `wds-call2` tool (retry after killing the stale
process). This appears to be a modem state issue from repeated reboots, not
related to D2QFIX.

## 13. The path forward

The data path requires replicating **rild's userspace 0x3f requests** (not the
kernel indication). Options:

1. **Run stock rild on Debian** — the most direct approach. rild
   (`/vendor/bin/hw/rild`) is a bionic binary with HIDL/logd dependencies.
   Could be run via a bionic chroot or with the staged system libs (the
   cam-shim32 precedent). Risk: fragile, HIDL HAL service needed.
2. **Recover rild's exact wire format** — the qrtr_0 log only shows the first
   8 bytes of each 0x3f message. The full payload needs either DIAG tooling
   or LD_PRELOAD of cap_qrtr_bionic.so into rild (both complex on stock).
3. **DIAG capture** — the modem's DIAG logs show all QMI processing
   (including why the endpoint descriptors are needed and what the modem does
   with them). Needs diag_mdlog + a DIAG parser.

The QMI_IPA registration fix (ns-lite v3) is **done and proven** — the
handshake, rules, and WDA all work. The data path is a separate, deeper
problem requiring the rild-equivalent userspace setup.

## 14. Safety record

- One kernel rebuild (image 306, kernel #124, D2QFIX) and flash.
- Multiple planned sysrq-b reboots (with prior sync) across the session.
- Two stock-flash sessions (user did the DL-mode dance; Debian rootfs at
  `/data/rootfs.img` survived both).
- One modem SSR attempt (`echo 1 > /sys/kernel/boot_modem/ssr` — path didn't
  exist, no effect).
- No driver unbind/rebind, no persistent harm to the modem.
- Data calls were dropped/re-established for tests (WDS bind timeout is
  recoverable with persistent retry).
- Two planned sysrq-b reboots (with prior sync) to activate the fix / test configs.
- Data calls (wds-call) were dropped/re-established for tests; no persistent harm.
- ns-lite v2 preserved at `/usr/local/bin/qrtr-ns-lite.py.v2` (rollback path).
