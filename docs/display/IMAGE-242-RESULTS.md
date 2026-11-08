# Image 242 — LLCC/SCID fully tested, rendering unchanged

Image: `artifacts/flash/242-halium-boot-a6xx-llccb2.img`
sha256 `3180d29c012c5a9a086c294c1fbd867cceccfd1de02bfb20d97af1eea92677d4`
Kernel `#49`, boot `4e7d89ff`.

## The macro bug (found live on 241, fixed in 242)

`module_param_cb(llcc_scid, &param_ops_bool, llcc_scid_set, 0644)` is wrong:
the 3rd parameter is **`arg`** (`kp->arg`), not a setter
(`include/linux/moduleparam.h:169-170`, `:221-228`). The function pointer
became the "bool storage", so `param_set_bool` wrote into executable text:

```
Unable to handle kernel write to read-only memory at ffffff80089b0ee8
pc : kstrtobool+0xa4/0xbc
lr : param_set_bool+0x24/0x34
```

The setter never ran (dead code). The oopsing task died holding
`mod->param_lock` (`kernel/params.c:550`), so every msm_drm param read blocked
in D forever. That one bug explains four mysteries at once: the sh segfault
(rc=139), 0 LLCCSCID lines, the phantom initial `Y`, and the wedged param
interface. Fixed with a proper ops struct (`.set`/`.get` + `&llcc_scid`).

## SCID programmed successfully on 242

```
LLCCSCID: step 1 getd
LLCCSCID: step 2 activate
LLCCSCID: activate ret=0
LLCCSCID: step 3 write
LLCCSCID: scid=12 cntl1=0x00c6318c written (no readback)
```

- `scid=12` matches `llcc-sm8150.c:69` (`"gpu", sid 12`).
- `cntl1=0x00c6318c` = 12 replicated 5x across 5-bit fields. Correct.
- Shell survived (`WRITE_RC=0`): the canonical address `0x3c02` does NOT hang,
  unlike 240's wrong `0xF008`.
- Bring-up clean (`hw_init ret=0`), SMMU attr accepted on both domains from
  boot, map hint active. The full CAF LLC routing (attr + hint + SCID) was in
  place during the render test below.

## Result: still zero

`dmabuf.py` (sound CPU-side oracle), run after the SCID write:

```
pixel[0,0]     = 0x00000000  (r=0 g=0 b=0 a=0)
nonzero pixels = 0 / 4096
```

**The LLCC/SCID hypothesis is TESTED and REFUTED.** The complete CAF LLC
behavior (domain attribute + map hint + slice activation + SCID, with the
a640-appropriate skips) changes nothing.

## Standing

| image | hypothesis | result |
|---|---|---|
| 234 | IOVA base (fence hang) | FIXED the hang |
| 235 | A640 clock values | applied, no change |
| 236 | LLC_NWA + NOEXEC | HARMFUL (broke SQE fetch), reverted |
| 237 | RB amsbc | applied, no change |
| 238/239 | backend observation | RB=1, backend engages and retires |
| 240 | inline SCID at wrong addr | HUNG bring-up (0xF008 unmapped) |
| 241 | defines fixed, SCID runtime-gated | infra only |
| 242 | full LLC routing (attr+hint+SCID) | TESTED, no change |

Remaining CAF-verified candidates in the write path: `CP_ME_INIT` mask
(`0x2f` vs `0x16f` + `pwrup_reglist`), GMU AO CGC registers. After those,
there are no more CAF-documented divergences in this area.

Also corrected along the way (do not revisit):
- Our GBIF-region defines were ×4-inflated (`0xf114`, `0x58`, `0x38e8`);
  canonical is `0x3c45`, `0x16`, `0x0e3a`. This voids 231/232's "GBIF doesn't
  answer" conclusion (measured at an unmapped address).
- `kgsl_mmu_get_bus()` SMMU-master claim was a misread (ifdef-gated, d2q
  takes the `&platform_bus_type` branch like us).
- dma-iopmu identity domain: refuted (real arm-smmu; 28 GiB IOVAs on
  11 GiB RAM; `arm-smmu.c:2352` overwrites the clamp both agents cited).
- `CP_SMMU_TABLE_UPDATE`: retracted in-corpus (v5.10+, needs adreno_smmu).
- `adreno_program_smmu_aperture()`: a640v1-only; we are patch 1.

## Open systemic issue (not GPU work, blocks iteration)

A kworker spins in `pm_runtime_work` since boot on recent boots, producing RCU
stalls that progressively wedge the system (loadavg 20, D-state pileups, ssh
flakiness). Starts right after GPU bring-up (~372s). Undiagnosed; predates the
SCID test. If it recurs every boot, every session has a ~30-minute healthy
window.