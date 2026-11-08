# CONSOLIDATED — getting off dead center on Adreno 640 / n9750

Date: 2026-10-16. Main-session synthesis of four subagent consults plus direct
source verification. Supersedes nothing; each consult remains the detailed record.

**Inputs:** `beam-PLAN.md`, `musespark-SILICON.md`, `PLAN.md` (mimo),
`kernel-RESIDUAL.md` (nemotron), `ADJUDICATION-cp-reg-to-mem.md`,
plus the pre-existing corpus.

---

## 1. The problem, stated so it cannot drift

Submission is byte-correct. Nested IB graph walks. SQE provably dispatches
`CP_DRAW`. Submission retires, fence lands, **no faults ever**. **CP→DDR writes
work** (`memptrs->fence` increments). **RB→DDR writes have never been observed,
in any configuration, ever.** sysmem (`RB_BLIT` clear) and GMEM modes fail
identically. All Mesa RT/viewport/blit descriptors are field-correct.

The entire remaining problem is one asymmetry: *the render backend produces no
memory write while the command processor produces writes.*

## 2. Consults that reached a negative

This is the honest headline: **three of the four angles closed with no smoking gun.**

- **kernel-RESIDUAL.md** — no kernel-side lead found. Ring programmed, SQE
  enabled, CP init byte-identical to upstream, interrupts armed, per-block clocks
  programmed, secure mode off. No `CP_SET_CONTEXT` anywhere; the kernel never
  saves/restores hardware render contexts — all GRAS/RB state comes from Mesa
  in the stream. CAF-vs-upstream submit-path differences exist (no
  `CP_SMMU_TABLE_UPDATE`, `CP_INDIRECT_BUFFER_PFE` vs `CP_INDIRECT_BUFFER`, no
  `a6xx_fenced_write` GMU poll) but **none explain RB→DDR failing while CP→DDR
  works**: fence writes prove SMMU translation works, no faults prove CP_PROTECT
  isn't blocking, IB fetch/execute proves PFE works.
- **ADJUDICATION** — the `CP_REG_TO_MEM` macro bit layout is **not** the cause of
  the old hard reset. Upstream emits the identical packet on every submit.
- **musespark-SILICON.md** — visibility culling ruled out from Mesa control flow;
  ADDR64 truncation ruled out; "A640 is a downclocked A630" refuted (different
  gen/fibers/ECO magics in Mesa's device table).

A negative this consistent across four independent angles is itself
information: the defect is not where a kernel bring-up normally looks.

## 3. Verified facts that constrain the answer

Directly checked by the main session, not recalled:

| Fact | Evidence |
|---|---|
| `a6xx_registers[]` dump list **omits GRAS (0x8000-0x840B) and RB/PS (0x8800-0x8C33) entirely** | `a6xx_gpu.c:3188-3213` |
| ...but **already includes `RBBM_STATUS` (0x0210) and `CP_APERTURE_CNTL_HOST` (0x0a00)** | same |
| This CAF tree has **no `a6xx_gpu_state_get`/`adreno_gpu_state_get` at all** | grep, zero matches |
| KGSL's bank-select recipe is on disk: `aperture = ((cluster & 0x7) << 8) \| (ctxt << 4) \| ctxt` | `/home/usb/ShinyKernel/Kernel/drivers/gpu/msm/adreno_a6xx_snapshot.c:1000` |
| `RB_DONE` is emitted **nowhere** by freedreno a6xx; sysmem path ends in plain `fd_wfi()` | grep of `fd6_*.c` |
| Device SQE fw is **32304 B / md5 `42b0a156…`**; linux-firmware master ships **34188 B / md5 `9f2540d7…`** | `musespark-SILICON.md` |
| Upstream `get_stats_counter()` emits `CP_REG_TO_MEM_0_REG\|CNT(2)\|64B` **unconditionally in `a6xx_submit()`** | upstream `a6xx_gpu.c:222-229, 372, 380` |

### The unresolved reconciliation

Corpus (222/223) says device fw blobs are byte-identical to **stock in
`vendor.raw.img`**. The staleness finding compares against **current
linux-firmware**. Both hold: the running blob is stock-for-this-boot-image but
older than what upstream ships today. This is why image 213's "swap in stock
SQE" was a no-op — stock *is* the old blob.

## 4. Recommended order of operations

Ranked by (decisiveness x cheapness) / risk. Items 1-3 require **no rebuild and
no flash**.

### 1. `RBBM_STATUS` during the draw window — cheap, but DEMOTED (see caveat)
`REG_A6XX_RBBM_STATUS = 0x210`, fields `RB_BUSY` (7), `CCU_BUSY` (8),
`A2D_BUSY` (9). **Already inside `a6xx_registers[]`**, so this may need no code
change — just read it inside the existing change-driven tracer at each
`pc`/`pkt` transition. RBBM is a **global** register, so it escapes the
context-banking limitation that made every previous CPU readback worthless.

**CAVEAT — this was largely answered already.** `IMAGE-239-RESULTS.md` already
sampled busy bits mid-blit and recorded `RB=1 CCU=1 UCHE=1 SP=1 HLSQ=1 VPC=1
CP=1 PC_DCALL=1`, with the target buffer's `0xAA` prefill **fully intact
(4096/4096)**. So the RB is not merely "armed" — it *engages, is busy, and
retires without storing anything*. The `RBBM_STATUS` read is therefore no longer
the branch point between "never armed" and "wrote nowhere"; it is a
cheap corroboration, best re-taken **in the `pc=0xd00` window** (IMAGE-239's
sample was during a blit, and the corpus has been burned repeatedly by
point-in-time reads).

The open question shifts downstream: *RB busy, engaged, no faults, no writes —
where do the stores go?*

### 2. Swap `a630_sqe.fw` — file copy + reboot
`/lib/firmware/qcom/a630_sqe.fw`: `42b0a156…`/32304 B -> `9f2540d7…`/34188 B.
Zero kernel risk, no flash. The one hypothesis that is (a) new, (b) untested,
(c) trivially reversible.

### 3. Userspace matrix + perf counters
One `FD_MESA_DEBUG` flag at a time, **stopping on the first non-zero RT**;
`FD_MESA_DEBUG=perfcntrs`; fullscreen / no-shader / textured probes to close
out the last cilling-shaped variant. Note `TU_DEBUG` does nothing on the
Gallium path — a prior consult retracted a whole round of conclusions over that.

### 4. Aperture-correct cluster readback (X1) — the one real rebuild
Add GRAS(cluster 3) and PS(cluster 5) reads across contexts, with sub-block
select via `RB_RB_SUB_BLOCK_SEL_CNTL_HOST` (0=RAC, 9=RBP), using the KGSL
formula above and restoring the aperture to 0 afterwards.
Ground truth to match: the stream demonstrably writes `GRAS_2D_DST_BR=0x003f003f`
and `RB_MRT_BASE=0x700025000`. If those values appear in *some* (cluster, ctxt)
view, Mesa's state **is** live and the defect is below register programming.

### 5. Only then: heavier options
Draw-level replay oracle (Beam and mimo both advise deferring; a negative stays
ambiguous via dangling references), `CP_SET_SECURE_MODE 0`, pivot to mainline
msm on the same firmware.

## 5. What NOT to do

- Do **not** CPU-MMIO the GRAS/RB clusters directly — context-banked, and it has
  already produced a confident wrong answer once.
- Do **not** rebuild the 266 capture instrument expecting the bit-layout fix to
  help. If revived at all, it needs a **dedicated** destination BO, allocated
  outside `a6xx_submit()`/`hw_init` to escape the 264/265 `struct_mutex`
  deadlock class. Never the `memptrs` tail — that is live driver state.
- No hand-built PM4 (standing project rule; two fabricated claims already).
- No allocation inside the submit path.
- Never declare a freeze without `ps -eo pid,stat,wchan,comm` first.
- Do not trust point-in-time samples; log transitions at `udelay(1)`.

## 6. Decision tree

```
RB busy, engaged, retires, buffer untouched  (the OBSERVED case, IMAGE-239)
   -> Mesa state likely IS live; defect is below register programming.
   -> Confirm with X1 aperture readback: is GRAS_2D_DST_BR=0x003f003f /
      RB_MRT_BASE=0x700025000 visible in some (cluster,ctxt) view?
         - live  -> focus on UCHE/CCU flush, write-back path, secure gate,
                    or the firmware swap (item 2) as the cheapest live lead.
         - defaults everywhere -> state never host-visible; the bank-select
            mapping is itself the kernel lead.

Hypothetically: RB never busy at pc=0xd00
   -> state not live after all; X1 aperture readback is the whole ballgame.

Firmware swap fixes it -> firmware delta. Close, document, stop.
Everything negative -> honest conclusion: submission correct, SQE dispatches
   draws, CP writes DDR, RB engages and produces no writes; cause unresolved.
   Consider mainline msm, and consider that this may not be reachable from
   this stack.
```

## 7. Open questions for the user

- Is there a **working Android/Halium build with KGSL** for davinci on disk?
  `/home/usb/ShinyKernel/Kernel/drivers/gpu/msm/adreno_a6xx_snapshot.c` implies a
  KGSL tree exists locally. If KGSL **renders** on this device, that is a
  powerful control: it isolates CAF-msm from the hardware. If KGSL also fails,
  the hypothesis space collapses toward hardware or the boot chain.
- Is there a **second device** (a640 known-good) available as control?