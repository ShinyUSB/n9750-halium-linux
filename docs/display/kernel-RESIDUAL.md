# KERNEL-RESIDUAL — residual kernel-side lead for "RB→DDR writes nothing"

Date: 2026-10-16 (env). Corpus newest-authoritative rule: sort by mtime,
newest wins; this file appended incrementally per hard rule 3.

Mission (this angle ONLY): find any residual kernel-side defect that could
leave the render backend unarmed / its DDR writes unobservable, given:

- submission is byte-correct (FINDING-SUBMISSION-IS-COMPLETE.md)
- nested IB graph walks; SQE dispatches CP_DRAW pc=0xd00 pkt=0x70388003
  (FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md)
- submission retires, fence lands, int0=0 cpint=0 (no faults)
- CP→DDR writes WORK (memptrs->fence increments)
- RB→DDR writes NEVER observed in any config (sysmem RB_BLIT clear and GMEM
  mode fail identically)
- all Mesa RT/viewport/blit descriptors field-correct
- UBWC, mshader ISA ruled out

CLOSED (do not re-open; see consult/*.md): GX clock domain (SOLVED via
send_acd=1, IMAGE-230/233); GDSC regulator (IMAGE-226); GMU firmware
byte-identical to stock (222/223); SQE version-gate refuted (SQE provably
walks graph); CPU MMIO of GRAS/RB clusters "unusable" due to context banking
(only true for DEFAULT bus mux — see PLAN.md §2); command-stream contents,
nested IB, IB sizes, ring execution, coherency, memptrs_iova,
CP_SQE_MEMBASE, RBBM_CLOCK_CNTL, ADDR64 truncation, visibility culling,
"A640 is downclocked A630"; CP_REG_TO_MEM encoding NOT the reset cause
(ADJUDICATION-cp-reg-to-mem.md).

VERIFIED / BELIEVED / UNVERIFIED separation is enforced throughout. No
register numbers fabricated.

---

## 0. Source layout of THIS tree (CAF fork, not mainline names)

Active tree: /home/usb/ShinyKernel-gpu-exp/Kernel/drivers/gpu/drm/msm/

The a6xx driver here is a v4.19-era CAF fork. Naming differs from upstream
mainline (which the PLAN §10 cites, v6.12):

- `a6xx_submit()` lives INSIDE `a6xx_gpu.c` (no separate `a6xx_submit.c`).
- `a6xx_cmdstream.c` does NOT exist as a separate file here; cmdstream emit
  helpers are in `a6xx_gpu.c` (a6xx_submit, a6xx_flush) and `msm_gem_submit.c`.
- `a6xx_irq.c` does NOT exist here; irq handling is in `a6xx_gpu.c`
  (a6xx_irq / a6xx_handle_interrupt, need to confirm) + `msm_drv.c` irq table.
- `msm_perf_events.c` does NOT exist here; `msm_perf.c` is the perf file
  (17842 bytes) — check it for perfctr-related RB arming.
- `msm_ringbuffer.c` / `msm_ringbuffer.h` exist (72 bytes shown; ring alloc
  only; arming happens in `adreno_hw_init()`).

This matters: every upstream file named in the assignment must be located
in THIS tree before diffing. Status below per file.

Already confirmed present in-tree:
- a6xx_gpu.c (137501 bytes, heavily instrumented images 128-266)
- adreno_gpu.c (20132 bytes): adreno_hw_init() at line 101 programs
  CP_RB_CNTL/CP_RB_BASE/CP_RB_RPTR_ADDR; adreno_show()/adreno_dump() read
  registers with plain gpu_read(), NO bank switch.
- adreno_gpu.h, a6xx_gpu.h, a6xx_gmu.c/h, a6xx_hfi.c/h
- msm_gpu.c, msm_gem_submit.c, msm_ringbuffer.c, msm_perf.c, msm_drv.c

Not yet confirmed: a6xx_submit (yes, in a6xx_gpu.c:948), a6xx_flush,
a6xx_cp_init, a6xx_irq, context save/restore hooks.

---

## 1. a6xx_gpu_state_get / adreno_gpu_state_get — banked-cluster reads

### 1.1 Does THIS tree even have these functions?

PLAN.md §10 (desk-verified against upstream v6.12) describes upstream's
`adreno_gpu_state_get()` reading every register with plain `gpu_read()` and
no bank/context switch, and notes upstream never solved context-banked CPU
reads. KGSL's recipe (the ONLY prior art) is:

```c
/* vendor KGSL, /home/usb/ShinyKernel/Kernel/drivers/gpu/msm/
   adreno_a6xx_snapshot.c:996-1001 */
aperture_cntl = ((cur_cluster->id & 0x7) << 8) | (ctxt << 4) | ctxt;
kgsl_regwrite(device, A6XX_CP_APERTURE_CNTL_HOST, aperture_cntl);
/* plain CPU regreads of the cluster's register ranges then land on that
   cluster+context; A6XX_NUM_CTXTS = 2 */
```

Cluster IDs (KGSL adreno_a6xx.h:19-24): FE=0 SP_VS=1 PC_VS=2 GRAS=3 SP_PS=4
PS=5. GRAS cluster covers 0x8000-0x840B (GRAS_2D_DST_TL/BR 0x8405/6,
GRAS_SC_VIEWPORT_SCISSOR_*); PS cluster covers 0x8800-0x8C33 (RB_MRT_* 0x8822+,
RB_2D_* 0x8C00+, RB_DBG_ECO_CNTL), with sub-block sel via
RB_RB_SUB_BLOCK_SEL_CNTL_HOST (0x8E3B: 0=RAC, 9=RBP).
Reference crash-dump values: CP_APERTURE_CNTL_HOST/SQE/CD all 0.

### 1.2 In-tree status: UNVERIFIED

This CAF tree has NO `a6xx_gpu_state_get` / `adreno_gpu_state_get` by those
names (grep found zero matches in adreno/). It has `adreno_show()` (debugfs,
non-functional in this tree per FINDING-CP-CONSUMES-RING-RB-WRITES-NOTHING.md
§2) and `adreno_dump()` (printk variant, adreno_gpu.c:493-510), both using
plain `gpu_read(gpu, addr)` over `adreno_gpu->registers[]` ranges with NO
aperture switch. So this tree never reads banked cluster regs from CPU at all
— the cpstate node (`a6xx_gpu_cpstate`) and the change-driven burst trace
`a6xx_trace_burst()` read CP/SQE/REG_A6XX_* globals only.

Question to answer: does the tree's cpstate/burst read any GRAS/PS cluster
register? If not, "CPU readback unusable" is by design, not by measurement.

**CORRECT BANK-SELECT SEQUENCE ON A6XX (KGSL recipe, vendor-proven):**
1. `gpu_write(CP_APERTURE_CNTL_HOST, (cluster_id<<8) | (ctxt<<4) | ctxt)`
2. For PS cluster also program `RB_RB_SUB_BLOCK_SEL_CNTL_HOST` (0=RAC, 9=RBP)
3. Plain CPU regreads of the cluster's register range
4. Restore `CP_APERTURE_CNTL_HOST = 0` (known-good default) afterwards

Usable on a640? CP_APERTURE_CNTL_HOST = 0x0A00 exists in this tree's
`a6xx.xml.h:498` and upstream kern-a6xx.xml:457 (typed PIPE[12:13],
CLUSTER[8:10], CONTEXT[4:5]). Never written by our driver. Upstream msm
never writes it either. So the sequence is KGSL-only prior art; applying it
here is a NEW kernel write, but read-mux restore-to-0 makes it low risk.

**PLAN.md §X1 experiment (aperture-correct cluster readback):** one kernel
rebuild; add GRAS(3)×ctxt{0,1} and PS(5)×ctxt{0,1}×subblock{0,9} reads of
GRAS_2D_DST_BR / RB_MRT_BASE_LO/HI / RB_2D_* after a submit, to the existing
`a6xx_gpu_cpstate` sysfs node (or new param-gated dump), with idle baseline
diff. Ground truth: submitted stream demonstrably contains
GRAS_2D_DST_BR=0x003f003f and RB_MRT_BASE_LO/HI=0x700025000
(FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md §1-2). If any
(cluster,ctxt) view shows them → state IS live; RB armed, writes nothing.
If all views show defaults → state never became host-visible (SQE-side
context routing, or never programmed).

STATUS: UNVERIFIED — not yet built/run. This is the #1 residual kernel lead.

### 1.3 Which registers select the bank — CP_APERTURE_CNTL_HOST? RBBM? SCATCH? GMU?

Per KGSL: CP_APERTURE_CNTL_HOST (0x0A00) is the bus mux selecting
(cluster, context) for host reads of context-banked clusters. No RBBM/SCATCH/
GMU bank select is involved for host-side cluster reads; GMU has its own
aperture (CP_APERTURE_CNTL_SQE 0x0A01, _CD 0x0A03) for firmware-side reads,
not relevant to CPU readback. Our driver never writes ANY of them; default 0
selects context 0 / the default cluster mapping, which is exactly what image
263's walk trace showed: default-context read of GRAS_2D_DST_BR = 0 while the
stream demonstrably writes 0x003f003f (PLAN.md §8 LIVE DEVICE ADDENDUM,
/root/walk-rb.txt seq=1).

Cite: PLAN.md §2, §8, §10; KGSL adreno_a6xx_snapshot.c:996-1001,
adreno_a6xx.h:19-24; this tree a6xx.xml.h:498.

---

## 2. CAF-vs-upstream diff — RB-arming surface

Files to diff (this tree vs upstream raw
https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/...):

| file | in-tree location | upstream URL | status |
|------|-----------------|--------------|--------|
| a6xx_gpu.c (a6xx_submit, a6xx_flush, a6xx_cp_init, a6xx_hw_init) | adreno/a6xx_gpu.c | .../adreno/a6xx_gpu.c | PENDING |
| adreno_gpu.c (adreno_hw_init programs ring, adreno_show/dump) | adreno/adreno_gpu.c | .../adreno/adreno_gpu.c | PENDING |
| msm_gpu.c (msm_gpu_init, msm_gpu_hw_init, ring setup) | msm_gpu.c | .../msm_gpu.c | PENDING |
| msm_ringbuffer.c (ring alloc only) | msm_ringbuffer.c | .../msm_ringbuffer.c | PENDING |
| msm_perf.c (perfctr; RB_BUSY counter enable?) | msm_perf.c | .../msm_perf.c | PENDING |
| msm_drv.c (irq table, ioctls) | msm_drv.c | .../msm_drv.c | PENDING |

Any of these that leaves the RB unarmed:
- ring never programmed (CP_RB_BASE/CNTL/RPTR_ADDR/WPTR) → CP fetches nothing.
  REFUTED in-corpus: RPTR chases WPTR, ring consumed (FINDING-CP-CONSUMES...).
- CP_SQE_CNTL never set to 1 → SQE never starts.
  IN-TREE: a6xx_hw_init writes CP_SQE_CNTL=1 (line 2668) — but needs
  confirmation it actually executes on boot path (not just instrumented build).
- CP_ME_INIT / cp_init never run → CP not initialized.
  IN-TREE: a6xx_cp_init called at a6xx_hw_init:2674; upstream
  a6xx_cp_init emits CP_ME_INIT 0x2f,0x03,0x20000000,0... byte-identical
  (PLAN.md §4 row, RETRACTION-SMMU-TABLE-UPDATE.md:107). Not a lead.
- No CP_SET_SECURE_MODE 0 from ring → trust gate possibly closed.
  IN-TREE: never emitted in a6xx (only a5xx_gpu.c:587). Upstream emits it
  post-zap (a6xx_gpu.c:1166). Live as X5 in PLAN.md §3, but conditional on
  X1 showing state NOT live + X2 failing. Not the primary residual lead.

STATUS: PENDING — actual `git diff`/`curl` diff not yet performed. This is
the #2 residual kernel lead. Fastest route: fetch upstream files and diff
against in-tree, scoped to RB-arming registers only.

---

## 3. Context create/restore — CP_SET_CONTEXT, context bank, GMEM/secure

UNVERIFIED. Need to grep this tree for:
- CP_SET_CONTEXT emission (ring packet that switches the hardware context)
- context bank switching on submit (per-submit context vs shared context 0)
- GMEM context setup (GMEM base/size, UCHE_GMEM_RANGE — already programmed
  in a6xx_hw_init:2471-2478)
- secure context (SECVID_TRUST_CNTL=0 written at a6xx_hw_init:2705, mode 0;
  no CP_SET_SECURE_MODE packet; no zap fw in tree per IMAGE-243)

Key question: does a context ever get saved/restored such that RB state
becomes live? In this CAF fork, all state is programmed by userspace packets
(Mesa emits GRAS/RB setup directly into the IB). The kernel does NOT save/
restore a hardware render context between submits (no context restore buffer
path beyond MSM_SUBMIT_CMD_CTX_RESTORE_BUF which is a no-op when
priv->lastctx == ctx — a6xx_gpu.c:1440-1442). So "context restore" is not the
mechanism here; RB liveness = registers programmed by the stream + hw_init
defaults.

If the stream programs GRAS_2D_DST_BR etc. and they don't land, the suspect
is context banking (SQE writes to a context the host aperture can't select)
or a block not clocked — NOT a missing context save/restore.

STATUS: UNVERIFIED — grep needed for CP_SET_CONTEXT / context bank in-tree.

---

## 4. a6xx_init_hw / caps differences touching RB/CCU/GRAS/A2D

IN-TREE a6xx_hw_init (a6xx_gpu.c:2305-...) programs (verified by reading):

- SECVID_TSB_CNTL=0, trusted base/size=0 (secure off)
- ADDR64 block: CP/VSC/GRAS/RB/PC/HLSQ/VPD/VPC/UCHE/SP/TPL1/RBBM_SECVID_TSB
  ADDR_MODE_CNTL all = 1 (image 128 restore; ADDR64 ruled out as cause but
  the writes ARE present)
- a6xx_set_hwcg(true)
- A640: GBIF_QSB_SIDE0-3=0x00071620, RBBM_GBIF_CLIENT_QOS_CNTL=0x3
  (non-A640: VBIF path)
- RBBM_PERFCTR_GPU_BUSY_MASKED=0xffffffff (all blocks contribute)
- UCHE write/trap/write-thru bases = 0xfffff000:0x0001ffff (L2 bypass
  disabled)
- UCHE_GMEM_RANGE [0x100000 : 0x100000+gmem-1]; UCHE_FILTER_CNTL=0x804;
  UCHE_CACHE_WAYS=0x4
- CP_ROQ_THRESHOLDS_2 = 0x02000140 (a640_roq2) / 0x010000c0; _1=0x8040362c
- RBBM_NC_MODE_CNTL = 1 only if a640_rbbm_nc param (default off)
- CP_MEM_POOL_SIZE=128; PC_DBG_ECO_CNTL=(0x300<<11); CP_AHB_CNTL=0x1
- RBBM_PERFCTR_CNTL=0x1; CP_PERFCTR_CP_SEL_0=PERF_CP_ALWAYS_COUNT
- GMU AO_GPU_CX_BUSY_MASK=0xff000000; power counter select/enable
- RB_NC_MODE_CNTL=0x12 (amsbc=1 for RB, image 237), TPL1/SP=0x04,
  UCHE_MODE_CNTL=2<<21
- RBBM_INTERFACE_HANG_INT_CNTL=(1<<30)|0x1fffff
- UCHE_CLIENT_PF=1
- CP_PROTECT_CNTL=0x3 + 27 CP_PROTECT(n) windows
- RBBM_INT_0_MASK=A6XX_INT_MASK
- adreno_hw_init(gpu) → programs CP_RB_CNTL, CP_RB_BASE, CP_RB_RPTR_ADDR
- a6xx_ucode_init(gpu) → SQE firmware
- cur_ring = gpu->rb[0]; CP_SQE_CNTL=1 (SQE enable)
- a6xx_cp_init(gpu) → CP_ME_INIT etc.
- SECVID_TRUST_CNTL=0x0 direct write (secure_probe mode 0)
- final OOB clear (unless hold_boot)

NOT programmed anywhere in hw_init (confirmed by full read of 2305-2784):
- CP_APERTURE_CNTL_HOST / _SQE / _CD (any bank select)
- RBBM_CLOCK_CNTL2 per-block enables (RB, SP, UCHE, TP) — only aggregate
  RBBM_CLOCK_CNTL read back in diagnostics; ruled out as cause by
  RESEARCH-ASSESSMENT-A640 but the per-block enables are never explicitly
  written here (they may come up enabled by default)
- No pwrup_reglist / per-block ADDR_MODE power-up sequence (CAF-vs-KGSL
  divergence noted in PLAN.md §4; not a lead per PLAN §6 #3)

STATUS: hw_init reads done; diff vs upstream PENDING. Caps struct
(adreno_gpu->info / a6xx_gpu caps) not yet examined for RB-related capability
flags that could gate RB clocking.

---

## 5. Interrupts

UNVERIFIED. irq handler location and RBBM_INT_0_MASK=A6XX_INT_MASK contents
need checking. CP_CACHE_FLUSH_TS (bit 20) is the fence interrupt; the corpus
says fence lands, so the irq path works for CP. No evidence irq masking kills
RB writes (RB has no interrupt dependency for memory writes).

---

## 5. FINDINGS — each investigated area

### 5.1 Bank select / a6xx_gpu_state_get / adreno_gpu_state_get — THIS TREE DOESN'T HAVE THEM

**VERIFIED.** Grep for `a6xx_gpu_state_get` / `adreno_gpu_state_get` in `adreno/` returns zero matches. This CAF fork (v4.19-era) predates the mainline crash-state machinery. Upstream v6.12's `adreno_gpu_state_get()` (fetched from torvalds/linux master) does exist and reads every register with plain `gpu_read()` and NO bank/context switch — confirming PLAN.md §10: upstream never solved context-banked CPU reads either.

**What this tree HAS instead:**
- `adreno_dump()` / `adreno_show()` (`adreno_gpu.c:493-510`, `adreno_gpu.c:441-468`): iterate `adreno_gpu->registers[]` with plain `gpu_read(gpu, addr)`, NO aperture switch.
- `a6xx_cp_state_read()` (`a6xx_gpu.c:3054-3106`, the `a6xx_gpu_cpstate` sysfs node): reads only `RBBM_STATUS`, `RBBM_STATUS3`, `CP_RB_CNTL/WPTR/RPTR`, `CP_SQE_CNTL`, `CP_INTERRUPT_STATUS`, GMU regs, `CP_TIMESTAMP`. **No GRAS/RB cluster registers at all.**
- `a6xx_registers[]` (`a6xx_gpu.c:3188-3213`), the range list `adreno_dump()` walks: covers CP register indices (0x0800-0x08f3, 0x0a00-0x0a03 incl. `CP_APERTURE_CNTL_HOST`=0x0A00, 0x0c00-0x0cd9, 0x0e00-0x0e39) and some upper ranges (0x8600-0x8637, 0x8e01-0x8e77, 0x9600-0x9fff, 0xa600-0xa630), but **NOT** the GRAS cluster range (0x8000-0x840B, incl. `GRAS_2D_DST_TL`=0x8405, `GRAS_2D_DST_BR`=0x8406) and **NOT** the RB/PS cluster range (0x8800-0x8C33, incl. `RB_MRT_BASE_LO`=0x8825, `RB_MRT_BASE_HI`=0x8826, `RB_2D_*`=0x8C00+). Source: `a6xx.xml.h:343,498,1565,2597,2909,2911`.

**KGSL recipe (the ONLY prior art, vendor-proven in the same repo):**
`/home/usb/ShinyKernel/Kernel/drivers/gpu/msm/adreno_a6xx_snapshot.c:996-1001`:
```c
aperture_cntl = ((cur_cluster->id & 0x7) << 8) | (ctxt << 4) | ctxt;
kgsl_regwrite(device, A6XX_CP_APERTURE_CNTL_HOST, aperture_cntl);
```
Cluster IDs (`adreno_a6xx.h:19-24`): FE=0 SP_VS=1 PC_VS=2 **GRAS=3** SP_PS=4 **PS=5**. `A6XX_NUM_CTXTS = 2`. Restore `CP_APERTURE_CNTL_HOST = 0` afterwards (known-good default; reference crash dump shows all three aperture regs = 0, PLAN.md §2).
PS sub-block sel via `RB_RB_SUB_BLOCK_SEL_CNTL_HOST` (0x8E3B): 0=RAC, 9=RBP.

**Which registers select the bank:** `CP_APERTURE_CNTL_HOST` (0x0A00; also `_SQE`=0x0A01, `_CD`=0x0A03, defined in this tree's `a6xx.xml.h:498` and upstream `kern-a6xx.xml:457` typed PIPE[12:13]/CLUSTER[8:10]/CONTEXT[4:5]). No RBBM/SCATCH/GMU bank select is involved for *host-side* cluster reads. GMU has its own aperture (0x0A01/0x0A03) for firmware-side reads — irrelevant to CPU readback.

**Usable on a640?** `CP_APERTURE_CNTL_HOST` exists in this tree's `a6xx.xml.h:498` and is never written. The sequence is KGSL-only prior art; applying it here is a new kernel write, but restore-to-0 makes it low risk.

**This is an OBSERVATION gap, not a confirmed write gap.** The RB is programmed by the CP from userspace command streams; the CPU simply cannot read back cluster state without the aperture switch. Image 263's walk trace already demonstrated this: default-context CPU read of `GRAS_2D_DST_BR` = 0 while the stream demonstrably writes `0x003f003f` (PLAN.md §8, `/root/walk-rb.txt` seq=1). The aperture switch does NOT explain why the RB doesn't WRITE — it explains why we cannot VERIFY whether the state is live.

### 5.2 CAF-vs-upstream diff — RB-arming surface

**VERIFIED by reading both trees.** Fetched upstream v6.12 `a6xx_gpu.c` and `adreno_gpu.c` from raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/. In-tree files located: `a6xx_submit()` is inside `a6xx_gpu.c:948` (no separate file); `a6xx_flush()` at `a6xx_gpu.c:397`; `a6xx_cp_init()` at `a6xx_gpu.c:2116`; `a6xx_irq()` at `a6xx_gpu.c:2978`; `adreno_hw_init()` at `adreno_gpu.c:101`. No separate `a6xx_cmdstream.c`, `a6xx_irq.c`, or `msm_perf_events.c` exist in this tree (`msm_perf.c` exists but is a userspace profiling buffer, not perfctr arming).

| Upstream (v6.12) | CAF (this tree) | Relevance to RB writes |
|---|---|---|
| `a6xx_submit()` calls `a6xx_set_pagetable()` on ctx switch: `CP_SET_PROTECTED_MODE 0`, `PERFCTR_SRAM_INIT_CMD`, **`CP_SMMU_TABLE_UPDATE` (TTBR0+ASID)**, `CP_MEM_WRITE`, `CACHE_INVALIDATE`, `CP_SET_PROTECTED_MODE 1` | No per-submit pagetable programming at all | CP→DDR fence writes work ⇒ SMMU translation works for CP. RB shares the same SMMU. Not an obvious RB-specific blocker. |
| `get_stats_counter()` ×2 per submit (CP_REG_TO_MEM packets) | None | Stats-only; irrelevant to RB writes. |
| `CP_INDIRECT_BUFFER` for cmd buffers | `CP_INDIRECT_BUFFER_PFE` | IB is fetched & executed (SQE walk proven). Not the blocker. |
| `a6xx_flush()` uses `a6xx_fenced_write()` — polls `GMU_AHB_FENCE_STATUS` | `a6xx_flush()`: `mb()` + `gpu_write(CP_RB_WPTR)` only | Write-completion vs power-collapse; not an RB-write-enable. |
| `CP_SET_SECURE_MODE 0` from ring after zap; direct `SECVID_TRUST_CNTL=0` with warning on -ENODEV | Direct `SECVID_TRUST_CNTL=0x0` write (secure_probe mode 0, `a6xx_gpu.c:2705`) | Functionally equivalent (secure mode off). X5 candidate, conditional. |
| `adreno_gpu_state_get()` crashstate with plain `gpu_read()` | `adreno_dump()`/`adreno_show()` + `a6xx_cp_state_read()`; neither bank-switches | Observation gap only (see §5.1). |
| `a6xx_set_ubwc_config()` computes RB/TPL1/SP/UCHE NC_MODE from `ubwc_config` data | Hardcoded: `RB_NC_MODE_CNTL=0x12`, `TPL1=0x04`, `SP=0x04`, `UCHE_MODE_CNTL=2<<21` (`a6xx_gpu.c:2537-2540`) | UBWC ruled out (TILE_MODE=0 on RT). IMAGE-244 tested hwcg table incl. TEX_FCHE → still zero. |
| `a640_hwcg` table: RB0=0x22222222, RB0_CNTL2=0x01002222, TEX_FCHE=0x00000222, UCHE=0x22222222, CCU0=0x00002220 | Same table present (`a6xx_gpu.c:1836-1883`), `a640_hwcg_a640=true` default (`a6xx_gpu.c:815-816`) | Per-block clocks ARE programmed. IMAGE-244 refuted hwcg as cause. |

**No smoking gun found** in the CAF-vs-upstream diff that would specifically block RB→DDR writes while allowing CP→DDR writes. The submit paths are functionally equivalent for the RB: ring programmed, SQE enabled (`CP_SQE_CNTL=1`, `a6xx_hw_init:2668`), CP initialized (`a6xx_cp_init`, byte-identical CP_ME_INIT to upstream), interrupts armed (`RBBM_INT_0_MASK=A6XX_INT_MASK`).

### 5.3 Context create/restore — no hardware context management in this tree

**VERIFIED:**
- No `CP_SET_CONTEXT` packet anywhere in the tree (single match at `a6xx_gpu.c:659` is a comment about context-banked registers).
- `MSM_SUBMIT_CMD_CTX_RESTORE_BUF` is a no-op when `priv->lastctx == ctx` (`a6xx_gpu.c:1440-1442`); otherwise falls through to `MSM_SUBMIT_CMD_BUF` and emits `CP_INDIRECT_BUFFER_PFE`. No hardware context save/restore.
- `priv->lastctx = ctx` after submit (`msm_gpu.c:785`) — bookkeeping only, no hardware context switch.

**All render state (GRAS/RB registers) is programmed by userspace (Mesa) directly into the command stream IB.** The kernel does NOT save/restore hardware render contexts between submits. RB liveness = registers programmed by the stream + `a6xx_hw_init` defaults. There is no kernel context bank switch on submit; all state targets the default context.

Upstream's equivalent (`a6xx_set_pagetable` → `CP_SMMU_TABLE_UPDATE` with TTBR0/ASID on context switch, plus `CP_RESET_CONTEXT_STATE` on a7xx) is absent here — but again, CP→DDR writes work so the page table is at least roughly correct.

### 5.4 a6xx_init_hw / caps — nothing disables the RB

**VERIFIED by full read of `a6xx_hw_init` (`a6xx_gpu.c:2305-2784`).** Programs (in order):
- `SECVID_TSB_CNTL=0`, trusted base/size=0 (secure off)
- ADDR64: all blocks `ADDR_MODE_CNTL=1` (`a6xx_gpu.c:2404-2427`)
- `a6xx_set_hwcg(true)` → per-block clocks incl. RB0, CCU0, TEX_FCHE, UCHE (`a6xx_gpu.c:1996-2114`, table at `:1836-1883`)
- A640: `GBIF_QSB_SIDE0-3=0x00071620`, `RBBM_GBIF_CLIENT_QOS_CNTL=0x3`
- `RBBM_PERFCTR_GPU_BUSY_MASKED=0xffffffff`
- UCHE trap/write-thru bases, `UCHE_GMEM_RANGE`, `UCHE_FILTER_CNTL=0x804`, `UCHE_CACHE_WAYS=0x4`
- `CP_ROQ_THRESHOLDS` (a640: 0x02000140/0x8040362c)
- `RBBM_NC_MODE_CNTL=1` only if `a640_rbbm_nc` (default off)
- `CP_MEM_POOL_SIZE=128`, `PC_DBG_ECO_CNTL`, `CP_AHB_CNTL=0x1`
- `RBBM_PERFCTR_CNTL=0x1`, `CP_PERFCTR_CP_SEL_0=ALWAYS_COUNT`
- GMU busy/power counters
- `RB_NC_MODE_CNTL=0x12` (amsbc=1, image 237), `TPL1/SP=0x04`, `UCHE_MODE_CNTL=2<<21`
- `RBBM_INTERFACE_HANG_INT_CNTL=(1<<30)\|0x1fffff`, `UCHE_CLIENT_PF=1`
- `CP_PROTECT_CNTL=0x3` + 27 `CP_PROTECT(n)` windows
- `RBBM_INT_0_MASK=A6XX_INT_MASK`
- `adreno_hw_init()` → `CP_RB_CNTL`, `CP_RB_BASE`, `CP_RB_RPTR_ADDR` (ring fully programmed)
- `a6xx_ucode_init()` → SQE firmware
- `cur_ring = rb[0]`; `CP_SQE_CNTL=1` (SQE enable)
- `a6xx_cp_init()` → CP_ME_INIT (byte-identical to upstream)
- `SECVID_TRUST_CNTL=0x0` direct write
- final OOB clear

**NOT programmed anywhere in hw_init:** `CP_APERTURE_CNTL_HOST` / `_SQE` / `_CD` (any bank select); per-block `CLOCK_CNTL2` explicit enables (only the aggregate `RBBM_CLOCK_CNTL` via hwcg table). No `pwrup_reglist` / per-block power-up sequence (CAF-vs-KGSL divergence, not a lead per PLAN.md §6 #3).

**Caps:** `adreno_gpu->info` / a6xx cap struct not yet examined for RB-gating flags. No `CP_SET_CONTEXT` or context-bank switching on submit.

**CP_PROTECT is not blocking RB writes:** `cpint=0` (no `CP_INT_REGISTER_PROTECTION_ERROR`), and the CP demonstrably writes cluster registers (the stream contains `GRAS_2D_DST_BR=0x003f003f`). If CP_PROTECT blocked those, we'd see a fault.

---

## 6. Remaining kernel-side leads and honest assessment

### Leads that remain (none confirmed as the defect):

| # | Lead | Evidence | Status |
|---|------|----------|--------|
| 1 | **X1 aperture readback**: verify GRAS/RB state is live in some (cluster,ctxt) view | Can't observe cluster state without aperture switch (`a6xx_registers[]` omits GRAS/PS ranges; `a6xx_cp_state_read` doesn't read them) | UNVERIFIED — the one build needed |
| 2 | **UCHE/CCU flush coherence**: state live but writes don't reach DDR | PLAN.md §5 names this as post-X1 candidate | UNVERIFIED, conditional on X1 |
| 3 | **X5 CP_SET_SECURE_MODE / trust gate**: per-domain gate blocking RB writes | Upstream emits `CP_SET_SECURE_MODE 0` post-zap; our tree writes `SECVID_TRUST_CNTL=0` directly | BELIEVED but deprioritized (conditional on X1+X2) |
| 4 | **Caps struct**: RB-gating capability flag in `adreno_gpu->info` | Not yet examined | UNVERIFIED |

### Leads that are CLOSED (do not re-open):

| Lead | Why closed | Citation |
|---|---|---|
| Per-block clock gating (RB0/CCU0/TEX_FCHE) | `a640_hwcg` table programs them; IMAGE-244 tested hwcg incl. TEX_FCHE → still zero | `a6xx_gpu.c:1836-1883`, IMAGE-244-RESULTS.md |
| CP→DDR path broken | fence increments, memptrs writes work | FINDING-SQE-EXECUTES... §; multiple |
| IB not fetched / SQE not running | pc=0xd00 CP_DRAW caught, fence lands | FINDING-SQE-EXECUTES... |
| SMMU translation broken | CP→DDR writes work | ROOT-CAUSE-DANGLING-IOVA etc. |
| CP_PROTECT blocking RB regs | cpint=0, no protection faults | FINDING-CP-CONSUMES... §1 |
| CP_REG_TO_MEM encoding | identical to upstream, production path | ADJUDICATION-cp-reg-to-mem.md |
| Context save/restore missing | No such mechanism in this tree; all state from userspace | §5.3 above |
| "a640 is downclocked A630" | refuted | PLAN.md ruled-out list |

### Honest assessment

**No kernel-side smoking gun has been found.** The CAF-vs-upstream diff shows no difference that would specifically block RB→DDR writes while permitting CP→DDR writes. The ring is programmed, the SQE is enabled, the CP is initialized, interrupts are armed, per-block clocks are programmed (RB0, CCU0, TEX_FCHE), secure mode is off, and no faults are ever raised.

The one genuine kernel-side gap is **observability**: the driver cannot read GRAS/RB cluster state from the CPU because (a) it never writes `CP_APERTURE_CNTL_HOST` to select a cluster/context, and (b) `a6xx_registers[]` doesn't even include the GRAS/PS register ranges. This is the KGSL recipe's territory (`adreno_a6xx_snapshot.c:996-1001`). But an observability gap does not explain *why* the RB doesn't write — it only explains why we can't verify whether it's armed.

**If X1 shows GRAS/RB state is live in some (cluster,ctxt) view and X2 (Turnip) also fails silently**, then there is no kernel-side lead remaining in this CAF fork that explains the defect. At that point the honest conclusion per PLAN.md §9 is that the defect is below the register-programming layer (UCHE flush semantics, CCU, a per-domain secure gate, or hardware), and the correct action is the mainline pivot (X6), not another register sweep.

**If X1 shows defaults in ALL (cluster,ctxt) views**, the state was never made host-visible. Since all state comes from userspace (no kernel context management), this would mean either the stream's GRAS/RB writes landed in a non-default context (SQE-side banking — but the CP writes cluster regs directly, not via SQE context) or the registers were programmed and then cleared. The aperture sweep is the discriminator.

APPEND ONLY from here; mark each new section VERIFIED/BELIEVED/UNVERIFIED with file:line or web citations.


| candidate | evidence | status |
|-----------|----------|--------|
| Cluster registers never bank-switched for host reads; "RB state unreadable" is by design | in-tree adreno_show/dump use plain gpu_read, no aperture; KGSL recipe exists in vendor tree in same repo | BELIEVED (verification by grep pending) |
| State never becomes live in a host-visible context (SQE writes to context 0/1 not default) | walk trace: default-context GRAS_2D_DST_BR=0 while stream writes 0x003f003f (PLAN.md §8) | UNVERIFIED — X1 aperture readback is the decisive test |
| RB block not clocked at submit time | aggregate clocks logged in diagnostics; per-block CLOCK_CNTL2 never written by hw_init; GX domain now ON (send_acd solved) | UNVERIFIED — per-block clock regs not yet read post-boot |
| No upstream-style CP_SET_SECURE_MODE / trust gate open | in-tree writes SECVID_TRUST_CNTL=0 directly; no zap; no CP_SET_SECURE_MODE packet | BELIEVED but deprioritized (X5, conditional) |
| CAF-vs-upstream diff leaves RB unarmed | not yet performed | PENDING |

No kernel-side lead is yet CONFIRMED as the defect. The honest interim
conclusion: **the single best-resourced residual kernel experiment is X1
(aperture-correct cluster readback)**; if it shows state live in some
(cluster,ctxt) view, the defect is below the register-programming layer
(clocking, UCHE flush, secure gate) and not a missing kernel arming step.
If it shows defaults in ALL views, the state was never made host-visible and
the bank-select sequence itself (or SQE-side context routing) is the kernel
lead to fix.

APPEND ONLY from here; mark each new section VERIFIED/BELIEVED/UNVERIFIED
with file:line or web citations.
