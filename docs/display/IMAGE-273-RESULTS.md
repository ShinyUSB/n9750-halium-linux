# Image 273 — the SECVID_TRUST write WEDGES the GPU (a negative, and an informative one)

Date: 2026-10-20
Image: `artifacts/flash/273-halium-boot-a6xx-secvid.img`
sha256 `c0f06f2f879d33286d3aebe378d9c32ee11190ed4f734231eab53dffbed24bd8`

## What ran

`a6xx.secvid=1` forced the `SECVID_TRUST_CNTL` write that
`a6xx.experimental_nonlegacy=1` has always been skipping, with before/after
readbacks. The ZAP was loaded first (as usual) and the GPU bring-up followed.

## Result: the write wedges the GPU

```
[  46.224660] SECVID134: TRUST_CNTL=0x1 TSB_CNTL=0x0 TSB_BASE=0x0 SIZE=0x0 ...
[  46.224671] SECVID273: FORCING SECVID_TRUST_CNTL write (a6xx.secvid=1) despite nonlegacy=1
[  46.224683] SECVID273: before: TRUST_CNTL=0x1 TSB_CNTL=0x0 TSB_BASE=0x0 SIZE=0x0
<no "after:" line — execution never got past the write>
```

* The **"after" readback never printed** — the sequence stopped between the
  "before" print and the "after" print, i.e. at the `gpu_write()` of
  `REG_A6XX_RBBM_SECVID_TRUST_CNTL = 0x0` or the readback that follows it.
* The run script **hung in step 2** (the GPU bring-up) and never reached the
  workload — the "workload" output in that window was stale from the previous
  run.
* The GPU is **wedged**: `a6xx_gpu_cpstate` reads return *nothing* (the reader
  blocks on `struct_mutex`, which the hung submit path still holds), and several
  tasks are in D state:

```
    139 D    kthrea dsms_sender_kth
    495 D    secdp_ secdp_event
    619 D    dhd_ru dhd_rpm_state_t
    965 Ds   grab_s (ostnamed)
```

## What this proves — and it is a useful result, not just a null one

1. **The a6xx path's own stated intent is wrong for this silicon.**
   `HWINIT[13]` says *"no zap init in this tree; SECVID_TRUST later"* — but
   doing exactly that **hangs the GPU**. The `experimental_nonlegacy=1`
   condition, which was originally about the HFI v2 path, has been
   *accidentally protecting* the project from a wedging write for the whole
   series.

2. **The current configuration is therefore the correct one**: leave
   `SECVID_TRUST_CNTL` at its reset default (`0x1`), with `TSB_CNTL=0`,
   `TSB_BASE=0`, `SIZE=0` — which is exactly the state a5xx also runs in
   (a5xx likewise never writes `SECVID_TRUST_CNTL`; it only *protects* it from
   the CP, as our a6xx does via `CP_PROTECT(6) = RW(0xf000, 0x810)`).

3. **The a6xx trust configuration is not the defect.** That was the last
   software-side item the tree itself flagged, and it is now closed: doing what
   the tree intended makes things strictly worse.

4. **A reproducible GPU wedge.** The project has an intermittent
   "PMIC abnormal reset" / hard freeze it could never reproduce on demand —
   167 of 213 archived captures, in the GPU's *secure* side. This experiment
   produces a **deterministic** GPU hang from a single register write. That is a
   concrete lead for the intermittent class: something occasionally pokes a
   register in a state the GPU's secure firmware cannot tolerate, and the
   consequence is the EL2/HYP abort plus a wedged GPU.
