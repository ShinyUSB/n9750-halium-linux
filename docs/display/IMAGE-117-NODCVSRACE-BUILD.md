# Image 117 (rebuilt) — build-only: full sequence + DCVS serialize

## Change vs prior 117 (`15d56325…`, superseded)
- `a6xx_hfi.c` v2 `a6xx_hfi_start`: restored canonical sends, no skips:
  `FW_VERSION` -> `PERF_TABLE` (v2 `a6xx_hfi_send_perf_table`) ->
  `BW_TABLE` (canonical Image-110 `a6xx_hfi_send_bw_table`) -> `ECP` ->
  `core_fw_start` -> `start`. Removed PERF-SKIP, BW-SKIP, stop-after-ECP
  early return. Each stage logs enter/ret and stops at first failure.
- `a6xx_hfi.c`: dropped `__maybe_unused` on `a6xx_hfi_send_perf_table`
  (now called).
- `a6xx_gmu.c`: fixed reset-path bracket broken in first 117 pass:
  `if (!ret) { bootstrap_active=true; hfi_start; bootstrap_active=false; }`.
  Resume path unchanged: flag brackets message-level `hfi_start` only;
  post-bootstrap `set_freq` unaffected. `set_oob` suppresses
  `GMU_OOB_DCVS_SET` while flag set (log `GPU_DCVS deferred…`, return 0,
  no write/poll; all in-tree DCVS callers ignore result). No OOB/HFI/
  queue/IRQ/firmware/clock/MMU/power changes.
- Kept: 109 ACK dispatch, 116 HWINIT markers, stage banners
  (`BEGIN/FW_VERSION/PERF_TABLE/BW_TABLE/ECP/END`).

## Complete bootstrap sequence (this image)
```
=== GMU BOOTSTRAP BEGIN ===        (resume, flag set)
=== FW_VERSION === -> ACK/timeout -> ret
=== PERF_TABLE === -> ACK/timeout -> ret (stop on fail)
=== BW_TABLE === (+ q0/q1 snapshot) -> ACK/timeout -> ret (stop on fail)
=== ECP === (+ q0/q1 snapshot) -> ACK/timeout -> ret (stop on fail)
=== GMU BOOTSTRAP END ===
core_fw_start -> ret; start -> ret; (flag cleared before set_freq)
-> msm_gpu_hw_init -> HWINIT[00..25]
```

## Build checks
- full build: EXIT 0, 0 errors; fast build: 0 errors, 0 warnings.
- SDE/dsi/samsung/display: 0. DT: only pre-existing 14-line gmu phandle.
  Production `ShinyKernel`: untouched (no files newer than 2026-10-02).
- ramdisk `eb5d2eb5…` verified; image 62 `e19c71fa…` untouched.
- Image 117 rebuilt: `117-halium-boot-a6xx-nodcvsrace.img`
  sha256 `2fb509f2edd608874c79e290bf78d4a45e1a010d29d45775dd86037f8ba7cff9`
  (replaces non-compliant `15d56325…`).

Note: prior 117 run's "DCVS race" was synchronous post-failure `set_freq`
after version timeout, not an async source (no async DCVS caller exists in
tree); gate is in place regardless. No flash performed; stopping.
