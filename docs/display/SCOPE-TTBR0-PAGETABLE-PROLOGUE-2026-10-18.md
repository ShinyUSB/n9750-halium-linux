# SCOPE: obtaining TTBR0 + ASID for CP_SMMU_TABLE_UPDATE

Date: 2026-10-18
Follows `FINDING-SUBMIT-PATH-MISSING-PAGETABLE-PROLOGUE.md`. Source-only.

## Verdict: tractable — roughly 30–40 lines, not 200

Every piece of infrastructure this needs **already exists in our 4.14 tree**.
There is no MMU porting work to do.

## What is already present (verified)

| requirement | present in our tree | location |
|---|---|---|
| `DOMAIN_ATTR_TTBR0` enum | ✅ | `include/linux/iommu.h:154` |
| `iommu_domain_get_attr()` | ✅ | `include/linux/iommu.h:424`, `:731` |
| SMMU computes TTBR0 **with ASID folded in** | ✅ | `drivers/iommu/arm-smmu.c:3627-3640` |
| `TTBRn_ASID_SHIFT` | ✅ = **48** | `drivers/iommu/arm-smmu-regs.h:235` |
| `CP_SMMU_TABLE_UPDATE` opcode | ✅ = 83 / 0x53 | `adreno/adreno_pm4.xml.h:208` |

`arm-smmu.c:3627`:
```c
case DOMAIN_ATTR_TTBR0: {
        val = smmu_domain->pgtbl_cfg.arm_lpae_s1_cfg.ttbr[0];
        if (smmu_domain->cfg.cbar != CBAR_TYPE_S2_TRANS)
                val |= (u64)ARM_SMMU_CB_ASID(smmu, &smmu_domain->cfg)
                        << (TTBRn_ASID_SHIFT);
        *((u64 *)data) = val;
```

So the SMMU hands back the single 64-bit value the packet wants, with the ASID
already in place. Splitting it is trivial:

```c
asid = (u32)(v >> 48);
ttbr = v & 0x0000FFFFFFFFFFFFull;
```

Our driver never calls `iommu_domain_get_attr()` for this, and never sets
`DOMAIN_ATTR_TTBR0` (which is correct — the SMMU derives it internally rather
than being told).

## Vendor reference — the exact packet shape for A6xx

KGSL **does emit it on this silicon class**, in
`drivers/gpu/msm/adreno_iommu.c`:

```c
static unsigned int _adreno_iommu_set_pt_v2_a6xx(struct kgsl_device *device,
                        unsigned int *cmds_orig, u64 ttbr0, u32 contextidr, ...)
{
        cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);
        cmds += cp_wait_for_me(adreno_dev, cmds);

        /* CP switches the pagetable and flushes the Caches */
        *cmds++ = cp_packet(adreno_dev, CP_SMMU_TABLE_UPDATE, 4);
        *cmds++ = lower_32_bits(ttbr0);
        *cmds++ = upper_32_bits(ttbr0);
        *cmds++ = contextidr;
        *cmds++ = cb_num;

        *cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 4, 1);
        cmds += cp_gpuaddr(adreno_dev, cmds, (rb->pagetable_desc.gpuaddr +
                PT_INFO_OFFSET(ttbr0)));
        *cmds++ = lower_32_bits(ttbr0);
        *cmds++ = upper_32_bits(ttbr0);
        *cmds++ = contextidr;
        ...
}
```

Comment worth quoting: **"CP switches the pagetable and flushes the Caches"** —
the vendor's own description of what this packet does.

Upstream msm emits the same packet with a different field packing
(`TTBR0_HI | ASID`, `CONTEXTIDR(0)`, `CONTEXTBANK(0)`), which is consistent:
upstream's `msm_iommu_pagetable_params()` stores ttbr and asid **separately**,
whereas our SMMU returns them **combined**. Same information, different
plumbing.

## The only real gap: getting the domain handle into a6xx_submit()

`a6xx_submit()` has `struct msm_gpu`, not an `iommu_domain`. Our
`msm_mmu_funcs` has no accessor for page-table params (upstream added
`msm_iommu_pagetable_params()` for exactly this).

Minimal shape:

1. Add one entry to `struct msm_mmu_funcs` — e.g.
   `int (*get_pagetable_params)(struct msm_mmu *mmu, phys_addr_t *ttbr, int *asid);`
2. Implement it in `msm_iommu.c` as a thin wrapper:
   ```c
   static int msm_iommu_pagetable_params(struct msm_mmu *mmu,
                   phys_addr_t *ttbr, int *asid)
   {
           struct msm_iommu *iommu = to_msm_iommu(mmu);
           u64 v;

           if (iommu_domain_get_attr(iommu->domain,
                                     DOMAIN_ATTR_TTBR0, &v))
                   return -EINVAL;
           if (ttbr)
                   *ttbr = (phys_addr_t)(v & 0x0000FFFFFFFFFFFFull);
           if (asid)
                   *asid = (int)(v >> TTBRn_ASID_SHIFT);
           return 0;
   }
   ```
3. Call it from `a6xx_submit()`, and on success emit the `CP_SMMU_TABLE_UPDATE`
   packet plus (optionally) the `CP_MEM_WRITE` of TTBR0 to the memstore.

## Caveats to respect before writing code

- **`TTBRn_ASID_SHIFT` lives in `drivers/iommu/arm-smmu-regs.h`**, a private
  IOMMU header that `msm` cannot include cleanly. Use the literal `48` with a
  comment citing `arm-smmu-regs.h:235`, or better, define a local constant
  rather than reaching across subsystems.
- **Do not fail the submit if the attribute is unavailable.** `arm-smmu.c`
  returns `-ENODEV` when the domain is not yet attached. Guard and log.
- **Do not emit unconditionally every submit.** Upstream guards with
  `if (ctx->seqno == ring->cur_ctx_seqno) return;` — the packet is for context
  *changes*. Emitting it per-submit would be wrong and would add ring traffic.
- **This touches the submit path.** The 264/265/266 history is entirely about
  what happens when you modify the submit path. The change here is additive
  (a new packets emitted before the existing ones) and must not alter the
  existing per-cmd loop, the IB handling, or the fence.
- **The memptrs CP_MEM_WRITE target** — upstream writes TTBR0 into the memstore
  "for debugging" and "needed for preemption". If our memptrs layout has no
  PT_INFO slot, skip it; it is not required for translation to work.

## Risk assessment

Lower than any previous experiment:

- No new register *writes* in the MMIO sense — this is command-stream
  generation, the same mechanism Mesa already uses successfully
- No hardware probing, no aperture, no sub-block selectors
- The packet is spec'd for A5XX-and-later and is emitted by the vendor driver
  on this silicon class
- Failure mode if wrong: the CP sees an unexpected packet. That is a
  worst-case hard reset, which is exactly the 266 risk class — so it still
  deserves a clean boot image and a known-good rollback, and should not be
  combined with any other change.

## Recommendation

Worth doing as a single, isolated change, on a known-good base, with the
rollback path already proven. It is the first hypothesis in the whole
investigation that:

1. is **shared** between RB and SP (forced by the compute result),
2. sits **below register programming** (forced by "state is live in silicon"),
3. has a **verified divergence** from both upstream and the vendor driver, and
4. costs ~30–40 lines with **no missing infrastructure**.

Recommended base: **image 263**, the known-good baseline — not 269, which now
carries the DRM-version and submitqueue-query changes. Keeping the variable count
at one matters more now than reusing the newest image.