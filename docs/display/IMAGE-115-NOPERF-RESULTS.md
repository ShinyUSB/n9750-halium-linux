# Image 115 — PERF processing blocks the GMU (A); hw_init entered, hangs

## Isolation result: A

With PERF skipped (BW already skipped):

```
verify_fw_version -> ACK -> ret=0
=== PERF_TABLE SKIPPED ===
=== BW_TABLE SKIPPED ===
feature_ctrl ECP -> ACK (id=11, error=0) -> ret=0
core_fw hfi_start (register) -> ret=0
fw_start end ret=0
message-level hfi_start -> ret=0
enter msm_gpu_hw_init -> calling gpu->funcs->hw_init
... no return marker; device later rebooted, no panic/oops on record
```

ECP is answered if and only if PERF was never processed. Strongest evidence
to date that **PERF_TABLE processing is the state transition after which
the GMU stops servicing HFI**.

## Standing

- HFI bootstrap (version/ECP/core/start handshake) completes through
  `fw_start ret=0` — first time the full attach sequence reaches hw_init.
- New frontier, recorded not fixed: hang inside `a6xx_hw_init`
  (`gpu->funcs->hw_init`, i.e. Adreno ring/IRQ setup against the live GMU).
- No SMMU fault, no panic from the GPU path itself.

Image 115 `713940e8f0164f7c97730cd8bf00f383474bf372a4b2607f43ee00481700fd26`.
SDE 0, DT +0, production untouched. Stopping.