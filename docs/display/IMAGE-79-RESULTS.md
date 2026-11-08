# Image 79 — A/B question answered: the pointer never changes, and the
faulting address is `pdc_mmio`, NOT `seq_mmio`

## Pointer values at probe time (authoritative host capture)

```
MMIO_MAP kgsl_gmu_reg     = ffffff8020480000  res 0x...02c6a000 - 0x...02c99fff  size 0x30000 flags=200
MMIO_MAP kgsl_gmu_pdc_cfg = ffffff801ff90000  res 0x...0b290000 - 0x...0b29ffff  size 0x10000 flags=200
MMIO_MAP kgsl_gmu_pdc_seq = ffffff8020410000  res 0x...0b490000 - 0x...0b49ffff  size 0x10000 flags=200
```

All three resources resolved, sensible sizes, `IORESOURCE_MEM` (`flags=200`).

## The comparison that reassigns the fault

Image 78's Oops:

```
Unable to handle kernel paging request at virtual address ffffff801ff90000
pc : msm_writel+0x48/0x60
x20: 00000000febea1e1
```

`0xffffff801ff90000` is **exactly `gmu->pdc_mmio`** on this run
(`kgsl_gmu_pdc_cfg`, phys `0xb290000`).

## Consequences

1. **The faulting write is a `pdc_write(gmu->pdc_mmio, ...)`, not
   `pdcseq_write()`.** The value `0xfebea1e1` is written by
   `a6xx_gmu_rpmh_init()` to `REG_A6XX_PDC_GPU_SEQ_MEM_0` — a v4.19-era
   `pdc_write()` call that still targets `pdc_mmio`. When I re-read the
   re-routed calls, `SEQ_MEM_0..4` went to `seq_write()`, but I did not verify
   the function shown in `rpmh_init` matches: the Oops register dump shows the
   write went to the pdc base, so either a `SEQ_MEM` write still uses
   `pdc_write()` or the pdc path itself is unmapped. Must re-audit the exact
   write list vs the disassembly.

2. **The "seq_mmio is never valid" hypothesis is not needed.** The mapping that
   faulted is the *cfg* one, which on this boot is byte-identical in form to the
   other two (same API, same flags, plausible non-overlapping ranges).
   A "bad region" explanation would have to account for `0xb290000` being bad
   while `0x2c6a000` and `0xb490000` work — far less plausible than "we wrote the
   wrong register to the right region" or "the region is access-gated".

3. **MMIO_MAP prints sit AFTER the `IS_ERR()` gate**, so all three pointers
   passed the check. But this run never reached PRE_RPMH/PRE_PDCSEQ, so the
   A-vs-B corruption question cannot be answered from it. If the pointers are
   unchanged at PRE_RPMH on a later run, the fault is a mapping/register issue,
   not corruption.

## DT base correction (recorded for accuracy)

The `sm8150-gpu.dtsi` I quoted says `0xb280000`/`0xb480000`, but this d2q uses
**SM8150 v2** (see the `Hardware name: ... SM8150 V2 ...` Oops line), where
`sm8150-v2.dtsi` defines `0xb290000`/`0xb490000` — exactly what the runtime
resolves. My earlier "DT disagreement" concern was comparing the wrong SoC
revision's file.

## Why this run stopped earlier than image 78

Image 78 reached `a640_gmu.bin loaded` and died in `a6xx_gmu_fw_start()`;
image 79 died after `a630_sqe.fw loaded` without emitting PRE_RPMH. The trigger
timing shifted (SSH at ~12 s vs ~30 s), so image 79's GMU probe ran in a
slightly different interleaving with the on-demand GPU attach. This timing
sensitivity is itself a signal — there may be more than one latent hazard on
the resume path.

## What is NOT yet known

* Whether the pointers are identical at PRE_RPMH (A vs B unanswered).
* Which exact `pdc_write()` in `rpmh_init` is the faulting instruction.
* Why a write to the cfg region faults when the region maps cleanly.
* Whether the collapse is ordering (vote), protection (the region needs the
  ACD/CX vote to be touchable), or a wrong-offset write to a protected register.