# Image 124 — BW never sent; PERF NACK burst tripped pre-existing BUG_ON → panic

## Bootstrap this run
- `FW_VERSION -> ACK (id=1)`. PERF (real votes, image-123 payload) → NACK
  (`id=0 size=3`, payload `02091b00 0000006f`, error 111). Stopped at first
  failure: **BW never sent** → no BW124-SEND/TIMEOUT lines by design.
- Our runtime BW state is therefore exactly the Image-110 hardcoded msg
  (1 level, ddr 0x50000/0x5003c/0x5000c ← 0x40000000, cnoc
  0x50034/0x5007c/0x5004c ← 0x40000000/0 + 0x60000001/0x20000001/0x60000001).
  q0/q1 before/after: n/a (not sent).

## New load-bearing finding: firmware multi-NACK burst + host BUG_ON
After the first NACK was consumed (rd 4→7), q1 held a burst of EIGHT
consecutive error packets (seq 1–7, payload word varies:
`0dbef2c0/0da50940/0da47740/0da474c0/0d8111c0…`, error always `0x6f`=111),
each preceded by an `ffffffbf` pad word. Packet seq 7 echoes our PERF
header (`00203b04`) in its payload. Full ring in
`oops-perf-nack-burst.log` (89 lines).
- Host `a6xx_hfi_queue_read` expects contiguous packets; rd=7 landed on a
  pad word `0xffffffbf` → declared size 255 > reader budget 19 →
  **pre-existing `BUG_ON(HFI_HEADER_SIZE(hdr) > dwords)`**
  (`a6xx_hfi.c:133`, upstream hardening, not 124 code) → Oops in
  `a6xx_hfi_task` (softirq) → `Fatal exception in interrupt` → reboot.
- 124 instrumentation is exonerated: its hooks sit in the BW-send path
  (never reached); IRQ counters are increment-only.

## PART 4 — vendor vs DRM table
| field | vendor KGSL | our DRM | verdict |
|---|---|---|---|
| bw_level_num | 13 (grp3d usecases) | 1 | DIFFERENT |
| cnoc_cmds_num | runtime TCS (2-usecase table) | 3 | DIFFERENT |
| ddr_cmds_num | runtime TCS (13-usecase table) | 3 | DIFFERENT |
| wait bitmasks | runtime `complete` flags | 0x01/0x01 | DIFFERENT |
| CNOC addrs | runtime TCS | 0x50034/0x5007c/0x5004c | DIFFERENT |
| CNOC data | runtime TCS | hardcoded (see above) | DIFFERENT |
| DDR addrs | runtime TCS | 0x50000/0x5003c/0x5000c | DIFFERENT |
| DDR data | runtime TCS | 0x40000000 | DIFFERENT |
| message size | `sizeof(hfi_bwtable_cmd)` (13×8 + 2×N) | 160 dwords | DIFFERENT |
| header | same format/plain H2F id 3 | same format, id 3 | SAME (format) |
- NOT byte-for-byte equivalent (stated explicitly).
- PART 5: vendor sends `bw_level_num=num_bwlevels` (13) with runtime
  RPMh-derived commands; ours sends 1 hardcoded level. Our
  `num_bwlevels/cnoc/ddr_votes`: nonexistent (no such structs — hardcoded).

## PART 3 (as observable)
- q1 at burst: rd=4 wr=28 → rd=7 wr=32 (+burst). No unmatched-ACK mystery:
  the NACKs *were* read; the reader died on framing, not matching
  (though NACK ret_id≠3 also never completes the waiter → -110 anyway).
- IRQ counts: exact numbers unavailable (124 counters print only on the
  BW-timeout path, never reached). HFI IRQ demonstrably fired repeatedly.
- SMMU faults: 0 before panic. Device rebooted, now alive (uptime 202s).

Image 124 `241bdcb0fd2c5889cd985f6c20917e21426d0c8140550773d39598680f228440`.
SDE 0, DT = 121, production untouched. Stopping.
