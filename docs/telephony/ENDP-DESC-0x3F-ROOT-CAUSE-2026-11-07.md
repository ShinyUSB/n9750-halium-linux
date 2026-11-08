# ENDPOINT-DESC (0x3f) INVESTIGATION — ROOT CAUSE OVERTURNED — 2026-11-07

**Device:** d2q (SM-N9750, SM8150), Debian / ShinyKernel 4.14.190.
**Companion docs:** QMI-IPA-REGISTRATION-FIX-2026-11-06.md (§11–13 superseded by this
report), TELEPHONY-DATA-PATH-2026-11-05.md.
**Decoder:** `qmi-ipa-fix/endp-desc-analysis/decode_full.py` (precise re-decoder;
byte-order-correct, service-map aware, prints type/txn/mlen/p0).

## TL;DR — three decisive results

1. **The seven stock `0x003f` messages are NOT IPA endpoint descriptors.** They are
   **UIM (QMI svc `0x02`) Logical Channel operations** sent by rild to the modem's
   UIM server: six channel OPENS (with AIDs of 13/16 bytes) and one CLOSE. The
   prior session's decoder mapped rild's port (`0x4b` that boot) to QMI_IPA because
   the message ID 0x3f numerically matches `QMI_IPA_ENDP_DESC_INDICATION_V01`.
   The port actually hosts UIM; every message in that conversation is a UIM
   message (READ_TRANSPARENT 0x20 ×17, READ_RECORD 0x21 ×7, GET_FILE_ATTRIBUTES
   0x24 ×35, REGISTER_EVENTS 0x2e, REFRESH_REGISTER 0x2a, LOGICAL_CHANNEL 0x3f
   ×7, AUTH-family 0x36/0x41) and the modem pushes UIM indications (CARD_STATUS
   0x32, SLOT_STATUS 0x48) on the same port. Wire-proof: a libqmi
   `--uim-read-transparent` request captured on the phone is byte-shape identical
   (req mlen 20, resp = result + read-result + card-result TLVs) to rild's
   observed 0x20 exchanges. IPA's endp-desc message never appears on stock in ANY
   capture (boot, call, or during-call windows).
2. **The Samsung modem firmware does not implement QMI_IPA msg 0x3f at all — and
   receiving it as an indication CRASHES the modem.** Live proof (this boot,
   image 306 / kernel #124 with D2QFIX):
   ```
   17:10:48 kernel: D2QFIX: sending RSC pipe ind (GRO_HW gate removed)
   17:10:48 kernel: modem subsystem failure reason:
             ipa_qmi_client.c:357:IPA Assert: 0 failed:
             ipa_qmi_client_ind_cb(): Invalid msg_id: 63.
   17:10:48 subsys-restart: Restart sequence requested for modem (RELATED)
   17:10:49 ipa3_lcl_mdm_ssr_notifier_cb: IPA received MPSS BEFORE_SHUTDOWN
   17:10:49 rmnet_ipa3 remove → re-probe → rmnet_data1 destroyed
   17:10:53 modem back up, all services re-announced (ns-lite handled it)
   ```
   **D2QFIX (image 306) is actively harmful**: every `SET_INGRESS_DATA_FORMAT`
   ioctl fires it, every firing SSRs the modem. This invalidates every data-path
   measurement taken on image 306 *after* the first ingress ioctl of that boot,
   and fully explains the "WDS bind intermittently times out on fresh boots"
   symptom (the modem was being crashed by the bring-up itself).
3. **The kernel-side IPA control plane on Debian is byte-identical to stock.**
   `ipa3_qmi_init_modem_send_sync_msg` values (platform_type 3, hdr_tbl 1672/1991,
   v4/v6 route 1288/7 + 1544/7, filters 776/1032, modem_mem 5104/4108,
   ctrl_comm_dest_end_pt 11, hash tables 1160/1416/648/904) match the stock
   capture line-for-line. The full handshake (INDICATION_REGISTER,
   INIT_MODEM_DRIVER_CMPLT, IPA_CONFIG 0x27, INSTALL_FILTER_RULE 13 UL rules,
   MASTER_DRIVER_INIT_COMPLETE_IND) completes identically. Stock rild performs
   **no** QMI_IPA client operations whatsoever — the kernel does all of it on
   both systems.

**Consequence:** the mission's working premise ("stock rild activates the modem
data path by sending 0x3f endpoint descriptors that Debian lacks") is **false**.
The modem data path must be re-opened from the remaining stock-vs-Debian deltas
(§5) — the leading one being the full 71-byte WDA SET_DATA_FORMAT that stock
sends (Debian sends a minimal variant), followed by the exact DL-rule pair
stock's ipacm installs.

---

## 1. Task A — the seven stock 0x003f messages, decoded

All seven: `TX DATA` from rild (comm ESAR) socket `1:0x42aa` → modem `0:0x4b`,
QMI **type 0 (REQUEST)**, first payload byte (TLV id) `0x01` (slot TLV). Each is
answered by a modem **RESPONSE (type 2)** with the same txn. Full table
(`endp-desc-analysis/seven-0x3f-table.txt`):

| # | ts (s) | txn | req mlen | resp mlen | interpretation |
|---|--------|-----|----------|-----------|----------------|
| 1 | 21.883 | 0x000f | 21 | 12 | UIM LOGICAL_CHANNEL **open** — slot TLV(0x01) + AID TLV(0x10, 13 B) |
| 2 | 23.340 | 0x0038 | 21 | 12 | open, AID 13 B |
| 3 | 24.211 | 0x003f | 24 | 12 | open, AID 16 B |
| 4 | 26.175 | 0x0042 | 21 | 12 | open, AID 13 B |
| 5 | 26.455 | 0x0043 | 24 | 12 | open, AID 16 B |
| 6 | 27.158 | 0x0044 | 24 | 12 | open, AID 16 B |
| 7 | 27.465 | 0x0045 | 12 | 7 | **close** — slot TLV + channel-id TLV(0x11); result-only response |

Size math (UIM logical channel, libqmi `data/qmi-service-uim.json`):
open = slot TLV (3+1) + AID TLV (3 + 1 + aid_len) → 21 (aid 13) / 24 (aid 16);
open response = result TLV (3+4) + channel-id TLV (3+1) → 12; close request =
slot (3+1) + channel-id (3+1) → 8-12 w/ vendor TLV; close response = result only → 7.
All fit exactly. (UIM "Logical Channel" = msg 0x3f — standard Qualcomm UIM,
used for ISIM/CSG/eUICC-style secure channels; Samsung rild opens them while
provisioning SIM state.)

### 1.1 Why the service is UIM and not QMI_IPA — the fingerprint

Modem port `0x4b` (stock boot) carried, from rild (ESAR/MBM threads):

| msg | count | UIM meaning | observed sizes fit |
|-----|-------|-------------|--------------------|
| 0x20 READ_TRANSPARENT | 17 req | EF reads | req mlen 20/22; resp 19..97 (EF content) |
| 0x21 READ_RECORD | 7 req | record reads | resp 21/45/59 |
| 0x24 GET_FILE_ATTRIBUTES | 35 req | file attrs | req 13/15; resp 7 (not found) .. 81 |
| 0x2e REGISTER_EVENTS | 2 req (MBM) | event mask u32 | **req 7, resp 14 (result + mask echo)** |
| 0x2a REFRESH_REGISTER(+vendor) | 1 req (QAR) | 256-B request | vendor refresh w/ file list |
| 0x36 / 0x41 | 1+1 req | auth/config family (Samsung ext.) | 12→11, 4→37 |
| **0x3f LOGICAL_CHANNEL** | **7 req** | **open×6 / close×1** | see §1 |
| ind 0x32 CARD_STATUS | 5 | unsolicited | 42..74 B (apps present) |
| ind 0x48 SLOT_STATUS | 3 | unsolicited | 74 B |

Debian-side cross-check: ns-lite's live cache maps **svc 0x02 (UIM) → port 0x4b**
on the current boot too — the modem assigns the same port to UIM across boots on
this device. The kernel's own IPA log proves the modem's QMI_IPA server was
elsewhere that boot: `ipa3_q6_clnt_svc_event_notify_svc_new: QMI svc:49 vers:1
ins:2 node:0 port:44` (0x2c). Nothing rild sends ever touches 0x2c.

### 1.2 The one factual residue from the old report — explained

- "rild sends 0x3f as a REQUEST (type 0), modem answers" — true, but it is UIM
  logical channel; the modem answering is normal UIM behaviour.
- "ad-hoc userspace type-0 request → error 57 (invalid message ID)" — correct and
  expected: 0x3f is not a request in QMI_IPA's message table on the modem's
  server. The modem was right to reject it.
- "type-4 indication to the server → error 1 (malformed)" — expected for the same
  reason.
- "the kernel's 0x3f indication (D2QFIX) is transmitted but does nothing" — worse
  than nothing: it asserts the modem (§2).

---

## 2. D2QFIX crash — the full mechanism (proven live)

`handle3_ingress_format` (rmnet_ipa.c:1670, image 306) unconditionally calls
`ipa_send_rsc_pipe_ind_to_modem()` → `ipa3_qmi_send_endp_desc_indication()` →
`qmi_send_indication(ipa3_svc_handle, &client_sq, 0x3f, …)` — an indication to
the modem's QMI_IPA **client**. The modem's `ipa_qmi_client_ind_cb()` has no
handler for msg_id 63 and executes an IPA Assert → modem SSR. The AP-side SSR
notifier (`ipa3_lcl_mdm_ssr_notifier_cb`, rmnet_ipa.c:3152) then unregisters and
re-probes rmnet_ipa3, destroying rmnet_ipa0/rmnet_data1 mid-bring-up.

Sequence this boot (exact kernel lines preserved in journalctl):
1. bring-up's `SET_INGRESS_DATA_FORMAT` ioctl → `D2QFIX: sending RSC pipe ind`
2. `modem subsystem failure reason: ipa_qmi_client.c:357:IPA Assert: 0 failed:
   ipa_qmi_client_ind_cb(): Invalid msg_id: 63.`
3. `subsys-restart: Restart sequence requested for modem, restart_level = RELATED`
4. `IPA received MPSS BEFORE_SHUTDOWN` → `ipa3_wwan_remove` → `rmnet_ipa3 started
   initialization` (re-probe) → rmnet_data1 gone (`OSError: [Errno 19]`)
5. modem reboot completes ~5 s later; ns-lite v3 re-caches and re-announces
   everything; the modem re-handshakes (INDICATION_REGISTER, CONFIG 0x27, 13 UL
   rules re-received — all confirmed in the ipa ipc log).

Note: after the SSR the modem **did** send `QMI_IPA_CONFIG_REQ` (0x27) — which it
had not sent at cold boot on Debian. On stock the modem always sends 0x27
(handled by `handle_ipa_config_req` → `ipa_mhi_handle_ipa_config_req`, a no-op on
this platform). Benign either way.

**Images 304/305 results are unaffected** (no D2QFIX there). Image 306's
"kernel sends 0x3f, data still doesn't flow" conclusion is invalid: the modem was
SSR-ing during those tests.

---

## 3. Task B — stock vs Debian, packet-by-packet (kernel side)

`stock-boot-ipa.log` (kernel IPA view, stock) vs the same log on Debian:

| step | stock | Debian (image 306, this boot) | verdict |
|---|---|---|---|
| INIT_MODEM_DRIVER_REQ contents | platform 3, hdr 1672/1991, rt 1288/7+1544/7, flt 776/1032, mem 5104/4108, ctrl ep 11, hash 1160/1416/648/904 | identical, line-for-line | MATCH |
| modem INDICATION_REGISTER | t=7.515 (resp 0/0) | t=11.330 (resp 0/0) | MATCH |
| modem INIT_MODEM_DRIVER_CMPLT | t=7.522 | t=11.337 | MATCH |
| kernel MASTER_DRIVER_INIT_COMPLETE_IND | "send indication to modem (1)" | same | MATCH |
| modem IPA_CONFIG 0x27 | present (no-op handler) | absent at cold boot, present after SSR | benign |
| modem INSTALL_FILTER_RULE (13 UL rules) | t=8.49 | present (13) | MATCH |
| ipacm DL rules (0x37, 1768 B, 2 rules, after a 0-rule clear) | t=27.2 + t=411 (call) | Debian's dl-rule: 1 rule, no clear-first, 1336-1768 B | **DELTA (DL only)** |
| modem FILTER_INSTALLED_NOTIF round-trip | present | present | MATCH |
| endp-desc 0x3f anywhere | **never** | kernel sends it (D2QFIX) → modem crash | **D2QFIX must go** |

Userspace deltas (from the qrtr_0 stock log; svc ids verified in source/libqmi):

| service | stock userspace behaviour | Debian today |
|---|---|---|
| WDA (svc 0x1A) | rild/MBM: **SET_DATA_FORMAT req mlen 71, first TLV 0x11 (UL agg proto)**, ×2 (two client ports 0x4223/0x4246); GET_DATA_FORMAT (0x21, mlen 11, p0 0x10) → resp 78; SET_CAPABILITY (0x30, mlen 18) ×2 → resp 7. Kernel kworker: SET_POWERSAVE_CONFIG (0x2D, mlen 18) + periodic SET_POWERSAVE_MODE (0x2E, mlen 4) | `wda_raw.py` sends a minimal request (dl/ul proto only); granted = plain QMAP, no aggregation |
| UIM (svc 0x02) | rild: full SIM provisioning incl. 7 logical-channel ops | n/a (ModemManager minimal) |
| DPM (svc 0x2F) | not visible in the boot window (call setup at t≈47–115 not captured) | qmicli open-port OK |
| WDS (svc 0x01) | profile ops visible; start/bind outside capture window | wds-call2 OK |

---

## 4. Task C — what rild actually does (RE summary)

- The seven messages come from rild's UIM client (thread ESAR; the REGISTER_EVENTS
  opener was thread MBM, one 0x2a from QAR) — standard Samsung RIL SIM
  provisioning. No rild function sends IPA endpoint descriptors; the "ESAR sends
  ENDP_DESC_INDICATION" claim was an artifact of decoding without the
  port→service map (the prior decoder's `svc_by_port` only ever learned 2
  services in that log, and none covered 0x4b).
- rild's IPA-relevant userspace role is the **WDA SET_DATA_FORMAT/GET/SET_CAPABILITY
  pair** (and Samsung's DCT drives the WDS call). The kernel (rmnet_ipa3 + IPA QMI)
  does everything else — identical on Debian.
- Concrete follow-up material for full RE lives in `qmi-ipa-fix/linux/`
  (headers), `netmgr/` (stock libqmi_cci.so / libqmiservices.so disassemblies) and
  the phone's /vendor tree; but no further RE is needed to close the 0x3f question.

---

## 5. Where the data-path investigation actually stands

Confirmed working on Debian (unchanged): ns-lite v3 registration, full QMI_IPA
handshake, 13 UL rules cached+installed, WDA exchange succeeds, DPM open, WDS
bind+start (IP assigned), DL rules accepted with FILTER_INSTALLED_NOTIF.

Still failing: modem bearer `rx_ok=0 tx_ok=0` (UL never reaches the bearer),
D2QRX=0 (no DL). With the 0x3f theory gone, the remaining **verified deltas** are:

1. **WDA data-format negotiation** (most promising): stock sends the full
   71-byte SET_DATA_FORMAT (starts with UL-agg-protocol TLV 0x11; per the netmgrd
   RE: QMAPV4 both dirs, dl agg 16384/31, ul agg 16384/32) plus GET and
   SET_CAPABILITY; Debian's minimal request gets "no aggregation" granted. The
   modem's UL framing/demux expectations follow this negotiation. The matching
   ioctls on stock are the agg variants (netmgrd computed edf=0x2e/idf=0x4e in
   one analysis; the deployed seq uses 0x1e/0x3e — needs reconciliation against
   the exact WDA grant).
2. **DL rules**: stock = 0-rule clear, then 2 rules (1768 B) built by ipacm with
   its own route tables (COMRTBLLANv4 tbl 10 churn, GENERATE_FLT_EQ ×2);
   Debian = single hand-built rule. DL-only impact.
3. **Everything else measured identical** (§3).

### Single most discriminating next experiment

Flash a kernel **without** D2QFIX (image 305 + ns-lite v3, or rebuild current
tree — it already has the original GRO_HW gate), then one clean bring-up with
full captures (qrtr_0, ipa ipc log, dmesg D2QQMI/D2QRX, WDA exchange via
`cap_qrtr.so`, ip4_rt/ip4_flt dumps, GSI stats) and **replicate stock's WDA
71-byte SET_DATA_FORMAT** (build it with a small C helper using the netmgrd-recovered
values) before the ioctls. Success criterion unchanged: WDS bearer `rx_ok>0` and
`ping -I rmnet_data1 8.8.8.8`.

If UL still fails with a stock-identical WDA grant, the next discriminator is a
DIAG capture of the modem's IPA (diag_mdlog) to see where UL frames die inside
the modem — the first modem-internal visibility point.

---

## 6. Recommended fix / actions

1. **Revert D2QFIX now (mandatory).** Two equivalent paths:
   - Flash `artifacts/flash/305-halium-boot.img` (kernel #123, debug, no D2QFIX),
     or
   - rebuild from the current tree (`/home/usb/mnt-sda/android-kernel`, which
     already contains the original `if (dev->features & NETIF_F_GRO_HW)` gate at
     rmnet_ipa.c:1669) as kernel #125 via pack-d2q.sh → image 307.
   Do NOT run any bring-up that includes `SET_INGRESS_DATA_FORMAT` on image 306
   — each run SSRs the modem.
2. **Keep** everything else: ns-lite v3, d2q-ipa-fw ordering, wds-call2, dl-rule.
3. Then execute §5's experiment.

## 7. Safety record this session

- One stock-seq run aborted by the D2QFIX-induced SSR (rmnet_data1 destroyed;
  recreated by hand afterwards). One deliberate egress-only ioctl run (no ingress
  → no D2QFIX → modem kept alive).
- No flashes, no modem NV/EFS changes, no driver unbinds initiated by the
  investigation. The SSR was caused by the already-deployed D2QFIX patch.
- One UIM read (EF 0x2FE2/ICCID) via qmicli for wire-proofing — read-only.

## 8. Artifacts

- `qmi-ipa-fix/endp-desc-analysis/decode_full.py` — byte-order-correct qrtr_0
  decoder (service-map aware).
- `qmi-ipa-fix/endp-desc-analysis/seven-0x3f-table.txt` — the seven messages.
- `qmi-ipa-fix/endp-desc-analysis/deb-qrtr0-current.log` — Debian qrtr_0 snapshot.
- Journal excerpts (D2QFIX + assert + SSR) — reproducible via §2 commands.
- libqmi UIM reference: mesa-root/root/libqmi-main-*/data/qmi-service-uim.json.

---

# ADDENDUM (2026-11-07 evening, image 305 + ns-lite v3, clean boot) — the real data-path failure

With the 0x3f red herring removed and D2QFIX reverted (305), the data path was
investigated end-to-end with the corrected understanding.

## What was replicated stock-faithfully and verified working

- **WDA SET_DATA_FORMAT**: stock sends a **71-byte request** (first TLV 0x11),
  TLV set `{0x10 qos, 0x11 LLP=RAW_IP(2), 0x12 ul=QMAPV4(8), 0x13 dl=QMAPV4(8),
  0x14 ndp, 0x15 dl_cnt=31, 0x16 dl_size=16384, 0x17 epinfo, 0x1B ul_cnt=32,
  0x1C ul_size=16384}` — replicated byte-exact (`qmi-ipa-fix/endp-desc-analysis/
  wda_stock.py`). **Accepted** (result 0); the modem grants QMAPV4 + UL agg
  16384/32. Requires DPM open-port first (InvalidOperation otherwise). GET
  (0x21, 11B) + granted set = 78B — matches stock exactly. SET_CAPABILITY
  (0x30) is rejected by the modem (err 71 InvalidQmiCommand) — not needed.
- **DPM**: enabled on the d2q (`fusion_sm8150_sdx50` block in netmgr_config.xml:
  `qmi_dpm_enabled=1`, agg 16384/31 + 16384/32, proto 8 both dirs, GRO+GSO=1).
- QMI_IPA handshake: byte-identical (verified again this boot).

## The UL failure, precisely localized

1. UL frames enter the IPA (rmnet_ip0 tx increments).
2. **They boomerang back to the AP**: captured raw on rmnet_ipa0 RX — our UL
   frames (QMAP hdr + IPv4) return via the **LAN exception pipe** (default
   route, default_pipe=10). The modem bearer never sees them (`rx_ok=0`).
3. The WAN producer pipe (ep 2) filter table is **empty in hardware**
   (ip4_flt_hw: no rules on ep 2).
4. The 13 Q6 UL rules the modem sends ARE received (D2QQMI RX
   INSTALL_FILTER_RULE) and cached — but the AP does NOT install them because
   **`qcom,modem-cfg-emb-pipe-flt` is set** (egress gate `num_q6_rules &&
   !emb_pipe_flt` + the `ipa_dp.c:1216` "modem cfg emb pipe flt" branch skip
   the install). On stock the SAME branch runs (same flag, confirmed in
   stock-boot-ipa.log:373) — so **on stock the modem installs the UL rules
   itself** (into the shared filter table); on Debian it does not.
5. With the flag's AP-side skip active on both, the differentiator is purely
   **modem-side**: the modem's WAN data path (its Q6 pipe + the UL rules it
   writes into the shared table) is never activated on Debian. Confirmed by the
   modem's own `GET_DATA_STATS` (QMI_IPA 0x30) returning **result-only, no
   pipes, no filters** — the modem reports zero active IPA data path.
6. Manual proof attempts (installing match-all / mux-id-matched UL rules into
   ep 2 routing to the Q6 table) all landed in HW but did NOT stop the
   boomerang — consistent with the modem's Q6_WAN_CONS pipe (ep 13) being
   down/unwired, not merely a missing AP-side rule.

## What this rules out / the remaining gap

- NOT: the 0x3f endp-desc (never sent on stock; crashes the modem on Debian).
- NOT: the QMI_IPA handshake, WDA format, DPM, WDS bind/start (all work).
- NOT: emb_pipe_flt (present on both).
- **The gap**: whatever tells the modem to bring up its Q6 WAN pipe and program
  its UL rules during call setup. This happens in the stock **call-setup
  window (t≈47–115 s) that is NOT covered by any existing qrtr capture** — the
  boot log ends at t=47.5 and the exchange log starts at t=115 with the call
  already up. The RIL logcat shows only `setupDataCall` (rild QmiWds, ~50 ms) —
  the modem does the IPA activation internally on its side.

## Next discriminator (needs a stock session)

Flash stock, then capture the call-setup window:
```
adb shell
su
svc wifi disable
cat /sys/kernel/debug/ipc_logging/qrtr_0/log > /sdcard/q0.log &   # stream
cat /sys/kernel/debug/ipc_logging/ipa/log  > /sdcard/ipa.log &    # stream
svc data enable    # let the call come up (~10-15 s)
sleep 20; kill %1 %2
```
Then diff the exact QMI sequence (esp. WDS bind content, DPM open content,
any QMI_IPA enable message) against Debian's bring-up. The 8-byte qrtr headers
are enough to fingerprint the message sequence; the ipa ipc log shows the
pipe/filter activation. Alternative without a flash: a DIAG capture of the
modem's IPA (diag_mdlog) during a Debian call attempt.

## Safety note

Image 306 (D2QFIX) must not be used — each ingress ioctl SSRs the modem.
The current tree is clean (no D2QFIX, no dtsi change — the emb-pipe-flt
removal was reverted after the stock flag was found present).

---

# ADDENDUM 2 (2026-11-07 late, STOCK call-setup captured on device) — the activation trigger

Fresh stock boot; captured qrtr_0 + ipa ipc logs across `svc data enable` with a
working call (ping 8.8.8.8 = 3/3). Files: `qmi-ipa-fix/endp-desc-analysis/stock-callsetup/`.

## The stock call-time activation sequence (from ipa.log)

The WAN pipes (ep 2 producer, ep 11 consumer) are configured at **boot**
(t=9.4, when netmgrd brings up rmnet_ipa0), not at call time. At call time
(t≈32.6) ipacm does, via `/dev/ipa` ioctls (decoded from the `cmd=c008cfXX`
trace):

| nr | ioctl | purpose |
|----|-------|---------|
| 16/17/18/32 | QUERY_INTF[_TX/RX/EXT]_PROPS | read the registered mux intf |
| 19 | GET_HDR | the QMAP mux header |
| 2  | ADD_RT_RULE ×several | build LAN-tether route tables (tbl 10/11) — not the WAN path |
| 34 | QUERY_RT_TBL_INDEX | resolve the WAN DL route table ("ipa_dflt_wan_rt" → idx 9) |
| 31 | GENERATE_FLT_EQ ×2 | build 2 DL filter equations |
| —  | WAN_IOC_ADD_FLT_RULE_EX → QMI_IPA_INSTALL_FILTER_RULE_EX (0x37, 1768 B) → **FILTER_INSTALLED_NOTIF result 0** | the 2 DL rules to the modem |

**DL data starts arriving 0.23 s after the FILTER_INSTALLED_NOTIF** (t=32.89)
and ramps (rx 1→28→90→122…). So the whole data path activates at the call-time
DL-rule install + route-table setup, on an up call.

## Verified byte-identical between stock and Debian now

- WDS bind_mux_data_port request: **15 bytes** = TLV 0x11 (mux=1) + TLV 0x10
  (endpoint: EMBEDDED(4), iface 1). Debian's wds-call2 frame matches exactly.
- WDA SET_DATA_FORMAT 71-byte request (accepted, QMAPV4+agg granted).
- QMI_IPA handshake. DPM enabled on this target.

## What Debian still lacks (the activation)

The 2 DL filter rules via `GENERATE_FLT_EQ` + the route-table plumbing. My
single hand-built dl-rule gets the notify but does not activate the path — the
stock sequence (0-rule clear, then 2 generated equations, correct table) is the
replication target. The exact 0x37 payload bytes still need a bionic
`cap_qrtr_bionic.so` capture (no aarch64 linker on the host; build on the phone
on Debian, capture on the next stock session) — but the sequence + structure are
now fully characterized, and the eq content is derivable from ipacm's
`install_wan_filtering_rule`/`add_dft_filtering_rule`.

---

# ADDENDUM 3 (2026-11-07, Debian test results + decision point)

Tested on Debian (305 + ns-lite v3, clean boot) with the stock call-setup
capture in hand:

| candidate fix | result |
|---|---|
| wds-call3 with APN="mts" | call UP, SAME IP as stock (10.167.223.172) — but bearer rx_ok=0 |
| dl-rule2 (0-rule clear + v4+v6 catch-all ROUTING via dflt_wan_rt) after call | 2 rules accepted (rc=0) — no activation, rx_ok=0, UL still boomerangs |
| DPM rx/tx both orders | (no change; also tested in prior sessions) |

So the structural replication does not activate the modem's WAN data path. The
byte-exact 0x37 content (the 2 GENERATE_FLT_EQ equations) is the remaining
unknown — it needs a `cap_qrtr_bionic` capture of ipacm on stock.

**`cap_qrtr_bionic.so` is now built on the phone** (`/root/cap_qrtr_bionic.so`,
native gcc, bionic-compatible, raw-syscall AF_QIPCRTR capture).

Next stock session (the definitive capture):
```
adb root
adb push /root-from-debian/cap_qrtr_bionic.so /data/local/tmp/
adb shell
setprop wrap.ipacm "LD_PRELOAD=/data/local/tmp/cap_qrtr_bionic.so"
stop ipacm; start ipacm          # or: kill ipacm (init restarts it wrapped)
svc wifi disable; svc data enable
# pull /data/ipacm log + /tmp/qrtr.cap
```
Caveat: ipacm logs to /dev/socket/ipacm_log_file→diag, and the wrap needs the
exact env; if wrap.ipacm doesn't take, run a manual `ipacm` with LD_PRELOAD
after killing the service (data re-establishes on restart).

Alternative flash-free path: enable the modem's IPA DIAG log mask over
/dev/diag during a Debian call attempt and parse the modem's own data-path
decisions.

---

# ADDENDUM 4 (2026-11-07, byte-exact stock DL-rule capture via LD_PRELOAD'd ipacm)

Built `cap_full.so` (raw-syscall LD_PRELOAD shim: AF_QIPCRTR send/recv + the
`/dev/wwan_ioctl` WAN_IOC_ADD_FLT_RULE_EX payload), placed at
`/vendor/lib64/cap_full.so` (the vendor linker namespace rejects
/data/local/tmp), wrapped `vendor.ipacm`. Captured the call-time 0x37 payload.

## The captured stock DL rules (the "2 rules" = 1768-B kernel 0x37)

Both rules are the **default WAN exception rules**, identical structure:
`{ip_type=v4, action=ROUTING, rt_idx=9 (ipa_dflt_wan_rt), mux_id=0xFF,
rule_id=0, hashable=1, eq: offset_meq_32 on v4-dst (offset 16)}`:
- rule 0: dst mask/value = `ffffffff/ffffffff` (limited broadcast)
- rule 1: dst mask/value = `f0000000/e0000000` (224.0.0.0/4 multicast)

These route broadcast/multicast DL to the AP — NOT the per-call data rule. So
the call-time activation is NOT driven by special rule content; the modem
accepts whatever valid rule and the path comes up with the call.

## Implication

The DL rules are accepted on Debian (notify round-trips) and are NOT the
activation blocker. The modem's WAN data path simply never activates on Debian
(modem GET_DATA_STATS empty) despite byte-identical handshake/WDA/bind/DPM/
WDS-start(+APN)/DL-rules. The UL producer-pipe filter table stays empty
(boomerang). The trigger is modem-internal and not visible in the AP-side QMI
message set — it needs the modem's own view (DIAG) or the full Android
telephony state (DSD/profile flow) that Debian doesn't reproduce.
