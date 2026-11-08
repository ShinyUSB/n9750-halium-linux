# Image 236 — HARMFUL, DO NOT FLASH. Revert to image 235.

**`236-halium-boot-a6xx-llcnwa.img` breaks GPU bring-up.** Retained only as
evidence. `hw_init` returns `-22` because the CP never drains the ringbuffer:

```
[654.914466] HWINIT[11] before a6xx_cp_init
[655.913235] [drm:adreno_idle] *ERROR* A640: timeout waiting to drain ringbuffer!
[655.913250] HWINIT[12] after a6xx_cp_init ret=-22
[655.913269] HWINIT[25] return ret=-22
```

Reproduced on two separate boots (`b726d433`, `dab79014`), so it is not
variance. The ring base and RPTR shadow program correctly
(`0x700001000` / `0x700000000`) and every GMU map succeeds — the CP simply
cannot fetch its command stream.

**Cause: `IOMMU_NOEXEC` added to the BO mapping flags in `msm_gem_vma.c`.**
CAF's `_get_protection_flags()` (vendor `kgsl_iommu.c:1919-1948`) has an
exception that was not ported:

```c
if (memdesc->priv & KGSL_MEMDESC_UCODE)
        flags &= ~IOMMU_NOEXEC;
```

CAF clears NOEXEC for microcode buffers. Image 236 marks the SQE ucode buffer
execute-never, so the GPU cannot fetch its instruction stream.

Lesson: a **partial** port of a function is worse than none. The exception
clause is exactly the part that encodes "this buffer is special", and it was
the part dropped.

The `DOMAIN_ATTR_USE_LLC_NWA` set in `msm_gpu_aspace_new()` was **refused**
(`-EBUSY`) and is a no-op — see below.

**Recovery target: `235-halium-boot-a6xx-a640clk.img`** — already built, no
rebuild needed. Known good: `hw_init ret=0` on boot `2ad57c27`.

---

Image: `artifacts/flash/236-halium-boot-a6xx-llcnwa.img`
sha256 `e3b746bec79d4214ea651cc3155e3bef1ed5ea7b753f5ce48496a25649d33353`
Kernel `#43`, boot `46f8b5e4`.

## What was attempted

Candidate #1 from `IMAGE-235-RESULTS.md` — the SMMU cache attributes CAF sets
for a640 but we do not:

1. `DOMAIN_ATTR_USE_LLC_NWA` on the `gfx3d_user` domain.
   CAF: `_enable_gpuhtw_llc()` (vendor `kgsl_iommu.c:1270-1296`) picks it for
   a640/a612/a680. Honoured by this SMMU driver at `arm-smmu.c:2282`
   (`IO_PGTABLE_QUIRK_QCOM_USE_LLC_NWA`) and `:3696-3698` (setter).
2. Mapping protection `IOMMU_NOEXEC | IOMMU_USE_LLC_NWA` added to
   `READ|WRITE`. CAF: `_get_protection_flags()` (`kgsl_iommu.c:1919-1948`,
   used at `:1961`).

## Result: the attribute was REFUSED

```
[442.497548] platform 2ca0000.qcom,kgsl-iommu:gfx3d_user:
             aspace 'gfx3d': DOMAIN_ATTR_USE_LLC_NWA = -16
```

`-16` is `-EBUSY`. `iommu_domain_set_attr()` returns it when the SMMU driver's
`set_attr` refuses — here almost certainly because the domain's page-table
format was already finalised by the time we set it, since
`msm_gpu_aspace_new()` runs after `iommu_attach_device()`.

**So the LLC hypothesis was never actually tested.** The `dev_info` added in
this image is what made that visible; without it the failure would have been
silent, exactly like the 234-233 situation.

The `IOMMU_NOEXEC|IOMMU_USE_LLC_NWA` map flags are still being passed, but
without the domain attribute they select nothing, so image 236 is
behaviourally identical to 235 for this hypothesis.

## Bring-up is fine

```
[442.498815] ACD230: AOP mailbox channel acquired
[442.511631] ACD230: '{class: gpu, res: acd, value: 1}' len=32 mbox_send_message ret=0
[442.516408] FORENSIC VERDICT GOOD BABEFACE ms~1
```

No freeze this time — the trigger was fired at ~442 s, past the 300 s
`gpu_init_delay_ms` deferral, per the `IMAGE-221-CLOCKS.md` §0 rule learned the
hard way on 235.

## Verification incomplete

`dmabuf.py` (the sound CPU-side oracle built in `MEASUREMENT-VALIDITY-AUDIT.md`)
did not complete on this boot. It succeeded on 235, and repeated EGL/GBM
init-teardown across many runs appears to leave the device's link unstable —
several consecutive ssh sessions returned no output despite `ping` succeeding at
~0.7 ms. Not attributed to the kernel change; the attribute was refused, so
236 differs from 235 only in a failed `set_attr` call.

**The render target is therefore not re-measured on 236.** No claim is made
either way from this image.

## Where #1 stands

To test the LLC hypothesis properly, the attribute must be set **before**
`arm_smmu_domain_finalise_s1()` builds the page-table format — i.e. before
`iommu_attach_device()`, not after. That means setting it on the CB device's
domain earlier in `msm_gpu_cb_get()`, or attaching the MMU lazily. This is a
structural ordering problem, not a one-line fix, and is why the change needed
its own image rather than a tweak.

## Next

Still untested, all CAF-verified:
- LLC attribute set **early** (above) — the real test of #1
- LLCC "gpu" slice activation + `GBIF_SCACHE_CNTL1` SCID (deliberately deferred
  from 236; CAF notes those registers sit on the CX/GX rail and need
  reprogramming on every power-up, so it is rail-timing-sensitive)
- `CP_ME_INIT` mask `0x2f` vs CAF `0x16f`, no `pwrup_reglist`
- `RB_NC_MODE_CNTL` amsbc bit `0x4` vs `0x14`
- GMU AO CGC registers (defines present, no writer)

## Note on the log channel (still unsolved)

`/proc/last_kmsg` is overwritten by the recovery boot; `/sys/fs/pstore` stays
empty because a hang writes no pstore. Every freeze therefore still costs a
reboot cycle and produces no forensic data. This remains the biggest obstacle
to iteration and was the second half of the requested work.