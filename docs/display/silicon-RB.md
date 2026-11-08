# CONSULT: silicon-level / RB-microarchitecture reasoning for "RB writes nothing"

Started: 2026-10-16 (session date per env; corpus uses "2026-11-16" labels for the
latest entries — both refer to the same recent work, newest files in
/home/usb/artifacts/gpu-bringup/).

Angle assigned: **why would the render backend produce zero memory writes while
the CP demonstrably writes DDR and no fault (int0/cpint) is ever raised?**
Focus: mechanisms producing a *silent no-op*.

## Rules I am following
- Grep the corpus for every mechanism before proposing it. Cite filenames.
- Separate VERIFIED / BELIEVED / UNVERIFIED strictly. Never invent register
  names/bits.
- Append incrementally; this file must stand alone.

(Sections appended below as work proceeds.)

---

## 1. Corpus triage — what the newest files establish (VERIFIED in-corpus)

Read in this pass (newest-first per mtime):

- `FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md` — clear + draw RT
  descriptors, viewport/scissor, `RB_2D_BLIT_CNTL` all field-correct
  (decoded against real XMLs in /tmp/opencode/regs). UBWC positively ruled out
  3 ways (TILE_MODE=0, RB_2D_DST_INFO FLAGS bit12=0, buffer size = 64*64*4
  with no metadata plane).
- `STATE-END-OF-SESSION-2026-11-16.md` — the master state table: CP→DDR
  writes work (fence), RB→DDR writes never observed; CPU MMIO readback of
  a6xx clusters unusable (context-banked); 3 attempts to read RB state via
  `CP_REG_TO_MEM` all device-broke (264/265 deadlock struct_mutex, 266 hard
  reset).
- `FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md` — change-driven ibtrace caught
  `pc=0xd00 pkt=0x70388003` (CP_DRAW) mid-flight. Names the **tile-visibility
  culling** idea (freedreno wiki quote about GROUP_IDs) as "worth testing
  first" — i.e. NOT yet tested, NOT refuted.
- `STATE-SUBMIT-PROVEN-RENDER-DEAD.md` — full render-path matrix (GMEM,
  sysmem, noubwc, nobin, notile, noubwc+sysmem): all retire, all zero.
  Notes `CP_SMMU_TABLE_UPDATE` never emitted; GMU AO CGC branch missing vs
  upstream; `a6xx_gpu.c:644` "RB never, CP/SP -> backend never engaged".
- `FINDING-SYSMEM-WEDGES-RB-PATH.md` — sysmem run hard-wedged the device
  once; sysmem is the only path that demands RB→DDR; RB_BLIT is the sysmem
  clear mechanism (fd6_blitter.c:796).
- `IMAGE-239-RESULTS.md` — **RB=1 SP=1 HLSQ=1 VPC=1 UCHE=1 CCU=1 CP=1
  PC_DCALL=1 busy bits seen during a blit**; prefill 0xAA test: buffer
  untouched; blit stream decoded, WFI+RB_DBG_ECO bracket+CP_BLIT present.
  So "RB never engages" is false — RB engages and retires with no traffic.
  Candidate named there: LLCC/SCID (later refuted on 242, see below).
- `IMAGE-242-RESULTS.md` — full CAF LLC routing (attr+hint+SCID 12,
  CNTL1=0x00c6318c) tested → still zero. REFUTED.
- `IMAGE-244-RESULTS.md` — CAF a640 hwcg table (46 entries incl. TEX_FCHE
  gates) tested → still zero. REFUTED. Cumulative refutation table inside.
- `RESEARCH-ASSESSMENT-A640.md` — A640-vs-A630 differences actually tested:
  hwcg table, GBIF_QSB_SIDE, RB_DBG_ECO 0x04100000 (correct a640 value in
  stream), RBBM_CLOCK_CNTL 0x8aa8aa82 (correct a640 value).
- `ROOT-CAUSE-SQE-NEVER-STARTS.md` (RETRACTED headline) — but keeps: the
  loaded `a630_sqe.fw` **fails upstream's a6xx_ucode_check_version() gate**
  (buf[0]=0, needs ≥0x190; 0x190 sits at dword[1]); our tree never calls the
  check. This stays live as a "wrong/stale SQE microcode" angle.
- `PLAN-GPU-ZERO-TO-FULL.md` — Phase B2 ranked table (secure mode, CP_ME_INIT
  divergence, UCHE-flush coherence, 64-bit truncation, sysmem/GMEM target).
  §5.1 retire list: dma-iopmu, CP_SMMU_TABLE_UPDATE, LLCC/SCID, amsbc,
  UB64/UBWC, unclocked-blocks, CPU↔GPU coherency — do not re-open.

### Already-refuted mechanisms I must NOT re-propose (with citations)

| mechanism | refuted by |
|---|---|
| UBWC on the RT | FINDING-MESA-EXONERATED (3-way) |
| tiling/GMEM/binning as cause | STATE-SUBMIT-PROVEN-RENDER-DEAD matrix |
| RB_DBG_ECO blit magic wrong/SKU mismatch | RESEARCH-ASSESSMENT-A640, IMAGE-239 decode |
| wrong RBBM_CLOCK_CNTL | ROOT-CAUSE-SQE-NEVER-STARTS (0x8aa8aa82 is A640's) |
| a640 hwcg table / TEX_FCHE | IMAGE-244 |
| LLCC/SCID/SCACHE routing | IMAGE-242 |
| LLC_NWA+NOEXEC | IMAGE-246/harmful, IMAGE-242 table |
| CP_ROQ_THRESHOLDS_2, GMU AO CGC+WFI_CONFIG, GBIF_QSB | IMAGE-244 table |
| UCHE_GMEM_RANGE halves clobbered | RESEARCH-ASSESSMENT-A640 R3 |
| sp_local/sp_pvt_gpuaddr | RESEARCH-ASSESSMENT-A640 R4 |
| CP_SMMU_TABLE_UPDATE | RETRACTION-SMMU-TABLE-UPDATE (v5.10+ only) |
| dma-iopmu identity domain | IMAGE-242 corrections, RETRACTION-DMA-IOPMU |
| CPU↔GPU coherency (uploads) | PLAN §2.3 (all BOs MSM_BO_WC), ALIAS3 |
| userspace buffer uploads invisible | ALIAS3-RESULTS (map(READ) proves data in pages) |
| Mesa RT/viewport/blit-descriptor fields | FINDING-MESA-EXONERATED |
| SQE never starts | ROOT-CAUSE-SQE-NEVER-STARTS retraction header |
| glClear/draw discarded by Mesa batching (era-188) | DRAWDISCARD + later: draws now provably submitted |
| `RB_CCU_CNTL` without WFI | RESEARCH-ASSESSMENT-A640 rank3: WFI present before CP_BLIT (IMAGE-239 decode) |
