# CONSULT — silicon-level / RB-microarchitecture: why RB writes nothing (musespark)

Session date (env): 2026-10-16. Corpus newest files labelled 2026-11-16; same recent work.
Device: Note10+ n9750 / SM8150 / Adreno 640 (a6xx). Debian 12 via Halium, custom CAF kernel.
Mesa: /tmp/opencode/mesafull. Regs: /tmp/opencode/regs/*.xml. Kernels: ~/ShinyKernel-gpu-exp, ~/a6xx-build, ~/ShinyKernel.

## Established facts accepted (with corpus proof)
- GLSL compiles; stream byte-correct; nested IB graph walks (FINDING-COMMAND-GRAPH-VERIFIED.md).
- SQE dispatches CP_DRAW: change-driven trace caught pc=0xd00 pkt=0x70388003, pc=0xeb pkt=0x70348003 (LOAD_STATE6_FRAG) (FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md).
- Submission retires, fence lands, rptr==wptr (STATE-SUBMIT-PROVEN-RENDER-DEAD.md, STATE-END-OF-SESSION-2026-11-16.md).
- CP->DDR writes WORK (memptrs fence 0->1->2) (same).
- RB->DDR NEVER observed, sysmem or GMEM (same + FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md).
- No faults ever: int0=0, cpint=0 (same).
- RT descriptors, viewport/scissor, blit-enable bracket, tile mode, pitch verified field-by-field vs real XML + Mesa emitter source (FINDING-MESA-EXONERATED...).
- UBWC ruled out (TILE_MODE=0 LINEAR both paths, FLAGS bit12=0, size exactly 16384) (same).
- mshader ISA ruled out: clear alone should write and does not (same, section "Honest gaps").
- CPU MMIO of a6xx clusters context-banked -> CPU reads different context than CP wrote; PERFCTR_GPU_BUSY_MASKED read constant 0x002f7fff while ALWAYS_ON advanced (STATE-END-OF-SESSION...).
- 11 instruments produced confident wrong answers (same).
- sysmem run once froze device (manual reboot, TRC lost - capture was dmesg ring, not file) (FINDING-SYSMEM-WEDGES-RB-PATH.md). GMEM path never wedges.
- CP_REG_TO_MEM attempts: 264/265 deadlocked on struct_mutex; 266 hard-reset GPU (STATE-END-OF-SESSION...).
- hw_init vs donor v4.19: every donor register written, 34 extras ours (CHECKS-HWINIT-DRAW.md) — BUT later STATE-SUBMIT-PROVEN notes ordering diff (SQE start before set_hwcg vs upstream last) + missing CGC_GLOBAL_LOAD_CMD/CGC_P2S_TRIG_CMD branch. Both claims on record; not reconciled by me yet.
- SQE ucode a630_sqe.fw fails upstream version gate (buf[0]=0, needs >=0x190; 0x190 at dword[1]) (ROOT-CAUSE-SQE-NEVER-STARTS.md, retained sub-finding).
- RBBM_CLOCK_CNTL 0x8aa8aa82 correct for A640 (0x8aa8aa02 is A630) (same).
- CP_SQE_MEMBASE hypothesis falsified: upstream never writes it either (same).
- CP_SMMU_TABLE_UPDATE (opcode 83) never emitted by our tree; counter-evidence: fence write lands so SMMU works for that addr (STATE-SUBMIT-PROVEN-RENDER-DEAD.md).

## Mechanisms to evaluate (assignment) — status
1. SQE draw culling on tile/bin visibility — OPEN, quoted in FINDING-SQE-... but tile/bin geometry check proposed, never done per corpus.
2. GROUP_ID / deferred state-fragment exec — OPEN, only the freedreno-doc quote exists.
3. RB write-back flush semantics — PARTLY TOUCHED: PLAN-GPU-ZERO-TO-FULL.md step 3 names UCHE-flush question; IMAGE-242 refuted LLCC/SCID only. RB_DBG_ECO_CNTL bracket verified correct. Whether RB needs explicit flush/finish for visibility: NOT resolved.
4. CCU/RB status bits, "render pending" — PROPOSED (FINDING-SQE-... next step 1) but never executed (no successful RB instrumentation on record).
5. A640 vs A630 RB differences — TOUCHED only for CLOCK_CNTL. "A640 is downclocked A630" claim itself UNVERIFIED.
6. Wrong SQE/CPU ucode silently mis-dispatching — OPEN lead (version gate fails). Needs primary-source check.
7. SCMD/banking — NOT FOUND in corpus greps so far; need to check what SCMD means here (possibly SQE command / state-command banking?).
8. Distinguishing registers given banked MMIO — PROPOSED, never executed.

## Rules followed
- Grep corpus for each mechanism before proposing; cite filenames.
- VERIFIED / BELIEVED / UNVERIFIED separated. No invented reg numbers/bits.

## Bottom line (read this first)

Symptom restated at silicon level: the SQE (firmware microcontroller that *is*
the a6xx command processor) provably fetches, decodes and dispatches
`CP_DRAW`/blit work, the ME-side epilogue (`CACHE_FLUSH_TS` timestamp into
memptrs) lands, no fault bit ever sets — and no RB/CCU data ever reaches DDR.
From the freedreno architecture docs (docs.mesa3d.org/drivers/freedreno.html,
"Hardware architecture" + "Pipelining (a6xx+)", fetched 2026-10-16) the RB is
separated from the CP by (a) per-cluster double-buffered register contexts the
CPU cannot read (M:N blocks-to-clusters; this is the documented reason CPU
MMIO readback of GRAS/RB is unusable — it corroborates the corpus finding),
(b) an independent CCU cache between RB and DDR whose contents reach memory
only via explicit `PC_CCU_FLUSH_*` events, and (c) a 2D engine with its own
context rollover. Every silent-no-op candidate below is a failure at one of
those three joints.

Ranked verdicts (detail + tests in sections below):

1. **Flush/drain never happens, WFI passes on idle-never-started HW — STRONGEST,
   fits all evidence including the single sysmem wedge.**
   `CP_WAIT_FOR_IDLE` waits for idle *signals*. If the RB/A2D never started,
   its idle signal already reads idle, WFI passes instantly, the submit retires
   cleanly, no fault fires, nothing is written. This is the only mechanism that
   explains a *clean retire with zero writes and zero faults* for the
   immediate-mode blit path (no vertices, no shaders, no GROUP_ID, no
   visibility, no binning — `fd6_blitter.c` emits straight PKT4 + `CP_BLIT`).
   The one historical sysmem wedge (FINDING-SYSMEM-WEDGES-RB-PATH.md) is the
   same mechanism with the opposite race: HW caught mid-state, WFI actually
   waits, hang-detect never gets to report because the capture died with the
   ring buffer.
2. **GROUP_ID / CP_SET_MODE / SQE-fw skew silently skips state fragments —
   PLAUSIBLE for draws, RULED OUT for the blit clear.** Independent finding
   this session: the loaded `a630_sqe.fw` is STALE (all local copies md5
   `42b0a156…`, 32304 B; current linux-firmware master is 34188 B, md5
   `9f2540d7…`, fetched and compared 2026-10-16). Upstream ships NO
   `a640_sqe.fw` (verified from the linux-firmware qcom/ listing — A640
   correctly uses `a630_sqe.fw`, so the tree's fw *name* is right). Crucially,
   stock firmware ALSO has `buf[0]=0x0`, i.e. it fails the version-gate
   reading quoted in ROOT-CAUSE-SQE-NEVER-STARTS.md exactly the way ours does —
   so that gate cannot be a real rejection criterion (it would brick all
   upstream A630/A640). The fw-age lead is therefore demoted for the blit path
   (REG_WRITE/BLIT/EVENT_WRITE/WFI opcodes are ancient) but stays open for
   GROUP_ID/mode-tag handling in draws. Decisive test is cheap: swap the fw
   file, no kernel build.
3. **Tile/bin visibility culling — RULED OUT for the failing configuration,
   from Mesa source control flow** (`fd6_gmem.c:use_hw_binning` requires
   `nbins>=2`; 64x64 is one bin; `fd6_emit_tile_renderprep`/`fd6_emit_tile`/
   `fd6_emit_tile_gmem2mem` all take the unconditional `fd6_emit_ib` branch
   when `!use_hw_binning`; sysmem forces `CP_SET_VISIBILITY_OVERRIDE=1` =
   treat-as-visible). A cull also cannot wedge; the wedge kills pure-cull
   theories outright.
4. **64-bit address truncation — RULED OUT at init** (all 12 `ADDR_MODE_CNTL`
   registers per the a6xx XML exist and the current tree writes all 12 =1;
   there is no 13th/A2D one — the 2D engine lives in RB/GRAS clusters). Residual
   risk (banked ADDR_MODE cleared on context switch) assessed negligible:
   these are RBBM-level config, not pipelined contexts.
5. **"A640 is a downclocked A630" — FALSE.** Mesa's own device table
   (`freedreno_devices.py`: A630=a6xx_gen1, A640/A680=a6xx_gen2) gives
   different fiber counts (gen2 = 4x), different ECO_CNTL magics (A640:
   TPL1 0x8000, GRAS 0x0, SP_CHICKEN 0x420, HLSQ 0x0, VPC 0x2000000,
   UCHE_0E12 0x1; A630 differs in 5 of 11), `has_z24uint_s8uint`/`multiview`
   only on gen2. Kernel spots that treat the part as A630 are therefore
   suspect on principle; the two concretely checked (RBBM_CLOCK_CNTL value,
   SQE fw filename) are correct for A640. `RBBM_CLOCK_CNTL` bit-7→RB-gate
   mapping that the tree comment asserts is UNSOURCED (no bitfields in any
   XML) — value correct, mechanism story unverified.
6. **Secure-mode/TZ gate on RB/CCU (PLAN rank 1) — consistent with everything,
   zero positive evidence, kept as last resort.** CP working + RB dropped is
   exactly its shape, but no measurement in the corpus touches it.
7. **"SCMD/banking"** — the term SCMD exists in NEITHER the corpus (zero hits)
   NOR the a6xx/pm4 XMLs (zero hits). Interpreted as SQE state-command
   dispatch + cluster context banking; covered under (2) and the register
   section below.

## What I read (corpus, newest-first) and what I take from each

- FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md — RT/viewport/blit
  bracket/tile/pitch verified vs real XML + Mesa emitter source. Sets the
  floor: stream bytes are not the bug. (Also corrects its own earlier UBWC
  error properly — three-way corroboration.)
- STATE-END-OF-SESSION-2026-11-16.md — CP→DDR works, RB→DDR never; banked
  MMIO; 11 dead instruments; CP_REG_TO_MEM deadlocks (264/265) / hard-reset
  (266). Defines what NOT to retry.
- FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md — pc=0xd00/CP_DRAW + pc=0xeb/
  LOAD_STATE6_FRAG trace; proposes GROUP_ID visibility-cull + RB
  instrumentation. This consult answers both proposals (cull ruled out §3
  below; instrumentation specified §6).
- FINDING-COMMAND-GRAPH-VERIFIED.md / FINDING-SUBMIT-PATH-PROVEN-SOUND.md /
  STATE-SUBMIT-PROVEN-RENDER-DEAD.md — graph/retire proven; hw_init order
  diff (SQE before hwcg) + missing CGC_GLOBAL_LOAD_CMD branch noted; SMMU
  counter-evidence (fence lands) noted.
- FINDING-SYSMEM-WEDGES-RB-PATH.md — the single most informative datum in the
  corpus for silicon reasoning (see §1 mechanism).
- CHECKS-HWINIT-DRAW.md — donor-hw_init faithful; PC_MODE_CNTL/RB_MODE_CONTROL
  never verified; KGSL-a640-init diff proposed, never done.
- CORRECTION-CACHE-COHERENCY.md → CORRECTION-BOUPLOAD-WORKS.md →
  ROOT-CAUSE-MEASUREMENT-READS-WRONG-MEMORY.md → BUFFER-IDENTITY-PROVEN.md →
  RESULTS-SESSION-258.md — the full measurement-chain saga; current trust
  rests on userspace dmabuf read of the locked front buffer correlated by
  size+inode+IOVA to the submitted RT. BELIEVED sound (two cache modes, same
  zeros; fence signals on the buffer).
- PLAN-GPU-ZERO-TO-FULL.md §5 — candidate ranking (secure-mode, CP_ME_INIT
  0x16f/vendor pwrup list, UCHE flush, addr64). Dispositioned per-item below.
- ROOT-CAUSE-SQE-NEVER-STARTS.md (retracted headline, sound sub-findings) —
  fw gate + RBBM_CLOCK_CNTL + SQE_MEMBASE falsification all reused/checked.

## Independent verifications performed this session (not corpus quotes)

V1. SQE firmware identity + staleness (files, bytes, network).
- All three local copies identical: `/tmp/opencode/a630_sqe.fw`,
  `/tmp/opencode/fwstock/a630_sqe.fw`, `/home/usb/mnt-sda/a6xx-build/fw/gpu-fw/a630_sqe.fw`:
  size 32304, md5 `42b0a1566c41917df56f15f7d9c7c479`, bytes[0:16] =
  `00 00 00 00 90 e1 6e 01 0b 1f 00 01 00 00 00 01`.
- `buf[0]=0x0, buf[1]=0x016ee190, buf[2]=0x01001f0b` — reproduces the corpus
  gate computation exactly (both arms false). VERIFIED.
- Current linux-firmware master `qcom/a630_sqe.fw` (fetched 2026-10-16 via
  kernel.googlesource): size 34188, md5 `9f2540d789d9fd4699a566d97fcabded`,
  `buf[0]=0x0, buf[1]=0x016ee207` — NOT identical to ours (ours is ~1.9 KB
  older/smaller), yet ALSO `buf[0]=0`. Hence the gate as quoted cannot
  discriminate good from bad firmware; the corpus's "fails upstream's gate"
  does not imply ours is rejected (upstream runs this blob family on working
  systems). The *staleness* is real; the *gate inference* is refuted.
- linux-firmware `qcom/` listing contains `a630_sqe.fw` and NO `a640_sqe.fw`
  (a650/a660/a702/a730/a740/gen8 all have their own; a640 does not). So the
  tree's `A640 → "a630_sqe.fw"` mapping (`adreno_device.c`) is
  upstream-correct. VERIFIED.

V2. A640 vs A630 are different GPUs (Mesa primary source).
- `src/freedreno/common/freedreno_devices.py`: A630 → a6xx_gen1
  (fibers_per_sp=128*16); A640+A680 → a6xx_gen2 (fibers_per_sp=128*4*16,
  supports_multiview_mask, has_z24uint_s8uint, has_dp2acc=False,
  has_8bpp_ubwc=False). Both num_sp_cores=2, num_ccu=2.
- Magic-reg deltas A630→A640: TPL1_DBG_ECO_CNTL 0x108000→0x8000,
  GRAS_DBG_ECO_CNTL 0x880→0x0, SP_CHICKEN_BITS 0x1430→0x420,
  HLSQ_DBG_ECO_CNTL 0x80000→0x0, VPC_DBG_ECO_CNTL 0x0→0x2000000,
  UCHE_UNKNOWN_0E12 0x10000001→0x00000001. RB_DBG_ECO_CNTL_blit identical
  (0x04100000), PC_MODE_CNTL identical (0x1f). Only 4 of Mesa's magics are
  actually emitted by freedreno (`RB_DBG_ECO_CNTL_blit` in fd6_blitter.c /
  fd6_draw.c, `TPL1_DBG_ECO_CNTL` in fd6_emit.c:1271, `PC_POWER_CNTL` in
  fd6_gmem.c); the rest are kernel-domain (unwrap: a wrong kernel ECO value
  is a kernel bug Mesa cannot paper over).
- Kernel check: A640 entry (`adreno_device.c:106-123`) has gmem=SZ_1M
  (correct: A640 GMEM is 1 MB), GMU fw `a640_gmu.bin`, zap `a640_zap.mdt`.
  `MSM_PARAM_GMEM_BASE` hardcodes 0x100000 — matches the UCHE GMEM-range
  programming in hw_init (0x100000 .. +gmem-1) and is the standard a6xx
  GMEM aperture. No bug here.

V3. Flush handshake = RB_DONE_TS + CP_WAIT_REG_MEM, then CACHE_FLUSH_TS
(Mesa primary source, `fd6_emit.h:fd6_cache_flush`).
- `fd6_cache_flush()` = RB_DONE_TS(timestamped) + CP_WAIT_REG_MEM(WRITE_EQ,
  poll memory for the RB_DONE timestamp) + CACHE_FLUSH_TS + CP_WAIT_MEM_GTE.
  So a full flush *waits in hardware* for the RB to report done.
- BUT the per-submit blit path (`fd6_draw.c:445-450`, `fd6_blitter.c`
  equivalent) does NOT call it: it emits PC_CCU_FLUSH_COLOR_TS +
  PC_CCU_FLUSH_DEPTH_TS + CACHE_FLUSH_TS + plain WFI. And sysmem_fini
  (`fd6_gmem.c:1650-1652`) likewise. Only the GMEM binning prologue calls
  the RB_DONE-waiting variant (`fd6_gmem.c:746`). Consequence: in sysmem,
  NOTHING in the stream verifies the RB did work — WFI passing is
  consistent with RB-never-started. This is the silicon core of hypothesis 1.
- Event numbers (envy pm4.xml, primary): CACHE_FLUSH_TS=4, RB_DONE_TS=22,
  PC_CCU_INVALIDATE_COLOR=25, PC_CCU_RESOLVE_TS=26, PC_CCU_FLUSH_DEPTH_TS=28,
  PC_CCU_FLUSH_COLOR_TS=29, BLIT=30, UNK_2C=44, UNK_2D=45, CACHE_INVALIDATE=49.

V4. CCU facts (docs.mesa3d.org, primary): "The CCU is the separate cache
used by 2D blits and sysmem render target access (and also for resolves to
system memory when in GMEM mode). Its memory comes from a carveout of GMEM
controlled by RB_CCU_CNTL" (XML: RB_CCU_CNTL @0x8e07, COLOR_OFFSET bits
31:23>>12, DEPTH_OFFSET 20:12>>12, GMEM bit22, CONCURRENT_RESOLVE bit2).
CCU tags mix attachment+layer; Mesa flushes/invalidates between uses.
Sizes (`freedreno_dev_info.h`): CCU_DEPTH 64 KB, CCU_GMEM_COLOR 16 KB →
ccu_offset_bypass = 2*64 KB = 128 KB; ccu_offset_gmem = 1 MB − 32 KB.
- Note (resolved, benign): UCHE_CLIENT_PF @0x0E19 is a single PERFSEL[7:0]
  field (mesa-a6xx.xml) — a UCHE *perf-counter client select*, not a
  functional enable. Mesa rewrites it to 4 in-stream every batch
  (fd6_emit.c:1283; reference logs confirm `PERFSEL = 0x4`), so the tree's
  hw_init write of 1 (a6xx_gpu.c:2546) is overridden per submit and cannot
  affect the data path. Closed, no action.

V5. Visibility/conditional-execution control flow (Mesa source, exact).
- `use_hw_binning()` (`fd6_gmem.c:247`): false unless
  `(maxpw*maxph)<=32 AND nbins>=2 AND num_draws>0`. 64x64 → 1 bin → FALSE.
- All three per-tile emitters take the unconditional branch when
  `!use_hw_binning`: renderprep `fd6_gmem.c:1271`, draw `fd6_gmem.c:1434`,
  gmem2mem `fd6_gmem.c:1473` (`fast_cleared || !use_hw_binning →
  fd6_emit_ib`, else `emit_conditional_ib` on VSC_STATE via CP_REG_TEST +
  CP_COND_REG_EXEC + PRED_TEST).
- sysmem_prep (`fd6_gmem.c:1603-1619`): SKIP_IB2_GLOBAL=0 (needs GLOBAL AND
  LOCAL for IB skip — disabled), SKIP_IB2_LOCAL=1, VISIBILITY_OVERRIDE=1
  (= force-visible; binning pass uses 1 while *generating* the stream,
  render pass uses 0 while *consuming* it — consistent polarity),
  RB_CCU_CNTL bypass offset, MRT immediate, RM6_BYPASS marker.
- Therefore the executed 64x64 stream contains NO visibility-gated packet
  on any path. Culling is out (see also: a cull cannot wedge).
- CP_SET_DRAW_STATE (mesa-pm4.xml): COUNT[15:0], DIRTY b16, DISABLE b17,
  DISABLE_ALL_GROUPS b18, LOAD_IMMED b19, BINNING b20, GMEM b21, SYSMEM b22,
  GROUP_ID b28:24; Mesa enable_mask ENABLE_ALL = BINNING|GMEM|SYSMEM,
  ENABLE_DRAW = GMEM|SYSMEM (fd6_emit.h:73-76). CP_SET_MODE payload selects
  which masks execute immediately vs deferred at draw time (XML doc notes
  0x1 ↔ ENABLE_MASK&0x6; exact bit correspondence is fw-version-dependent —
  which is precisely why stale SQE fw stays suspect for *draws*).

V6. RBBM_STATUS is globally readable and has the bits the corpus needs
(mesa-a6xx.xml @0x210): RB_BUSY b7, CCU_BUSY b8, A2D_BUSY b9 (the 2D/blit
engine), RAS b6, TSE b5, VSC b20, UCHE b17, CP_BUSY b2. RBBM space is NOT a
pipelined cluster — the corpus already samples RBBM_STATUS routinely
(=0x1 idle). The 261 ibtrace burst does NOT sample it (pc/pkt/rptr/wptr/
scratch/int0/cpint/fence/rptraddr/flush_ts only). Adding one MMIO read per
transition is new, safe (no allocation, no stream edit, no reset path —
unlike 264/265/266), and directly answers "RB never armed vs armed
elsewhere". Full protocol in §6.

## Mechanism-by-mechanism analysis (assignment checklist)

M1. SQE draw culling on tile/bin visibility → RULED OUT for this failure.
- Corpus status: proposed (quote) in FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md
  §"worth testing first"; tile/bin geometry check never performed.
- Silicon verdict: the check is now unnecessary — V5 above proves no
  visibility-gated packet executes in the 64x64 configuration on EITHER path,
  and a cull cannot produce the observed wedge. The freedreno doc sentence
  ("if the draw call is going to actually execute (some primitive is visible
  in the current tile)... executes the corresponding fragment") describes
  per-tile draw dispatch, which for a 1-bin target reduces to unconditional
  dispatch. Per-primitive degenerate-geometry culling (zero-area triangles
  from bad vertex data) remains OPEN for the *draw* path only — but it
  cannot explain the vertex-free blit clear, so it cannot be the common
  cause. Do not spend on this except via the RBBM_STATUS test (a culled draw
  shows CP-side activity with zero RB/CCU/A2D busy bits — same signature as
  "never dispatched", distinguished by the fw-disassembly test below).

M2. GROUP_ID / deferred state-fragment execution → OPEN for draws, OUT for blits.
- Corpus status: only the doc quote; never tested. Corpus DID verify
  LOAD_STATE6_FRAG executes (pc=0xeb) — but that is the immediate descriptor
  load, not GROUP_ID fragment execution. Whether dirty GROUP_ID fragments
  execute at CP_DRAW was never observed.
- Silicon reasoning: at CP_DRAW the SQE must reconcile CP_SET_MODE against
  each CP_SET_DRAW_STATE enable_mask (BINNING/GMEM/SYSMEM). The XML doc's
  mode↔mask note is explicitly tentative ("seems to"), i.e. the mapping is
  fw-internal and version-sensitive. Our SQE fw is ~2 KB older than current
  stock (V1). If its mode handling predates the mask tag scheme Mesa 22.3.6
  emits, fragments silently never execute → GRAS/RB clusters keep reset/stale
  state → draw launches against an unprogrammed backend → zero pixels, zero
  faults, clean retire. No CPU-visible trace exists by design (cluster
  banking). The blit clear uses immediate PKT4 + CP_BLIT with no GROUP_ID
  involvement, so this mechanism cannot be the common cause — but the draws
  were proven to *dispatch* (pc=0xd00), not to *configure*. Both halves:
  (i) swap fw to stock 34188 B blob and re-run (file swap, reboot, no
  build); (ii) disassemble our fw (Mesa qrisc-disasm) and audit the 0xd00
  routine's fragment loop + the 0xeb LOAD_STATE6_FRAG handler for mode-gated
  early exits.

M3. RB write-back flush semantics → central, specified as experiments.
- Corpus status: PLAN §5.3 raises UCHE-flush; IMAGE-242 refuted LLCC/SCID
  only; RB_DBG_ECO_CNTL bracket verified correct; "does the RB need explicit
  flush" never resolved.
- Silicon reasoning (V3+V4): RB data (blits AND sysmem draws AND GMEM
  resolves) transits the CCU; DDR visibility requires the in-stream
  PC_CCU_FLUSH_*_TS events to execute AND the CCU→DDR drain to function.
  The sysmem/blitter stream contains no RB_DONE handshake — retirement
  proves the SQE walked past the packets, not that the CCU drained. Two
  sub-cases split cleanly with GPU-side evidence only:
  (a) flush timestamps (RB_DONE_TS/PC_CCU_FLUSH_* are *timestamped* event
  writes into Mesa's control BO) land but RT=0 → backend wrote elsewhere or
  drain misdirected (address/offset bug class);
  (b) they never land → CCU handshake never completed (drain-stall class;
  consistent with the wedge).
  CPU readback of these timestamps is via the same memptrs/control mapping
  the fence already uses — no new allocation, no CP_REG_TO_MEM. If the
  control BO is not currently dumped, that is the missing instrument.

M4. CCU/RB status bits, "render pending" → ANSWERED (usable), unexecuted.
- RBBM_STATUS (0x210, global): RB_BUSY b7 / CCU_BUSY b8 / A2D_BUSY b9 /
  RAS b6 / TSE b5. Sample on every ibtrace (pc,pkt) transition during the
  0xd00/CP_BLIT window. Expected signatures: blit clear → A2D_BUSY+CCU_BUSY
  pulse; draw → RAS+TSE+RB+CCU. Absent pulses = never dispatched (fork is
  above RB: SQE→backend launch, clocking, or secure gate). Pulses without
  timestamps (§M3b) = started, never drained. Pulses with timestamps but
  RT=0 = wrote elsewhere.
- "Render pending" as a single bit does not exist in the XML; the busy-bit
  vector IS the pending state. PERFCTR_GPU_BUSY_MASKED reading constant
  0x002f7fff while ALWAYS_ON advances (corpus) means that counter is
  mis-programmed/disabled on this tree, not that the engine is idle — do
  not use it; use the RBBM_STATUS vector.
- DBGBUS (RB_0/RB_1 0x20/0x21, CCU_0/1 0x28/0x29, RBP 0x14, A2D 0x11) is the
  escalation if busy bits pulse but more granularity is needed; it needs
  setup + decode and is second priority.

M5. A640 vs A630 architectural RB differences → "downclocked A630" REFUTED;
concrete deltas listed (V2). Consequences for this bug:
- Any kernel value copied from an A630 table without checking the A640
  selector is suspect. Checked: RBBM_CLOCK_CNTL (tree now keys on
  adreno_is_a640 → 0x8aa8aa82; matches CAF selector quoted in-tree and
  upstream's else-branch) — value VERIFIED, but the comment's "bit 7 = RB
  gate" story is UNSOURCED (no bitfields exist in any XML) — treat the value
  as correct and the story as folklore.
- hwcg: the tree's a640 table programs ONLY index-0 blocks (SP0/TP0/RB0/
  CCU0) while the legacy table programs SP0-3/TP0-3. Mesa says A640 has
  num_sp_cores=2, num_ccu=2; on-device readback says SP1/RB1/UCHE clock
  addresses return 0xdeadbead (unimplemented). OPEN question (cheap):
  read back the CCU1/SP1 clock regs specifically — deadbead = unimplemented
  (fine, matches CAF programming only CCU0), 0x0 = implemented-but-never-
  programmed (suspect: a live second CCU/SP with gated clocks on the RB
  data path). A gated-but-needed block usually HANGS rather than no-ops,
  so this ranks below M1–M3, but it is one MMIO read.
- Missing upstream `a6xx_set_hwcg` CGC_GLOBAL_LOAD_CMD/CGC_P2S_TRIG_CMD
  branch (STATE-SUBMIT-PROVEN §2): still open, still kernel-cheap to port;
  it programs global clock-load sequencing that plausibly affects whether
  backend blocks exit reset. Ranked behind the zero-build tests.

M6. Wrong SQE/CPU ucode silently mis-dispatching → partly answered (V1).
- SQE fw NAME correct for A640 (no a640_sqe.fw exists upstream). FW AGE is
  real (32304 vs 34188 B) but the version-gate inference is refuted (stock
  fails it identically). GMU fw (`a640_gmu.bin`) and zap were verified
  elsewhere in corpus (GMU-FW-VERIFIED.md, ZAP-REAL-MECHANISM.md) and are out
  of my angle. CPU ucode (protected-mode? PFP-size?) is not an a6xx concept
  — no CPU-side ucode dispatches draws on a6xx; the SQE fw is the only
  microcode in this path. Remaining test: fw swap + qrisc-disasm audit (§M2).

M7. SCMD/banking → SCMD does not exist (zero hits, corpus + both XMLs).
- Interpreted and covered: SQE state-command (GROUP_ID) dispatch = M2;
  cluster context banking = M4 (why CPU MMIO fails + which registers escape
  it: RBBM_STATUS/CP_SQE_STAT/CP_RB_* are RBBM- or CP-space = global;
  GRAS_/RB_/SP_/VPC_/VFD_/PC_/HLSQ_ clusters = banked = CPU-unreadable).
  Rule of thumb for future instruments: if the offset is < 0x1000 (RBBM/CP)
  it is CPU-readable; if it is a cluster register it is not.

M8. Registers distinguishing "armed but wrote elsewhere" vs "never armed"
without trusting CPU MMIO → three GPU-side signals, no banked reads:
- (i) RBBM_STATUS busy vector during the draw window (M4) — never-armed =
  silent; armed = pulses.
- (ii) Timestamped event-writes already in the stream (M3): RB_DONE_TS /
  PC_CCU_FLUSH_*_TS sequence numbers in Mesa's control BO — armed+finished
  = present; never-armed or never-finished = absent. Correlate against the
  fence seqno that provably lands.
- (iii) Write-elsewhere detector: scan ALL submitted BOs (not just the RT)
  for the clear colour/pixel pattern after a blit. A truncated/misdirected
  write (e.g. low-32-bit aliasing) lands SOMEWHERE mapped — the corpus's
  "no blue dword anywhere" scan (RESULTS-SESSION-258 item 17) gestures at
  this but rested partly on the non-aliasing kernel reader; redo it through
  the trusted userspace dmabuf mappings. If pixels appear in a wrong BO,
  the class is addressing, not enablement.

## Proposed experiments, ranked (actionable, no conversation needed)

E1. RBBM_STATUS-vector walk trace (zero-build-or-one-line; do FIRST).
- Extend the existing change-driven ibtrace burst (a6xx_trace_burst, image
  261+) to also read REG_A6XX_RBBM_STATUS (0x210) on every (pc,pkt)
  transition. Cost: one MMIO read per transition; no allocation, no stream
  change, none of the 264/265/266 failure modes. Run the standard 64x64
  sysmem clear submit; decode bits 9/8/7/6/5 (A2D/CCU/RB/RAS/TSE).
- Decides: pulses never appear → dispatch fork above RB (go E2/E4); A2D or
  RB+CCU pulse but RT=0 → backend ran (go E3); pulses + wedge → drain stall.
- This is the instrument FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md asked for,
  minus the unusable banked registers (GRAS/RB framebuffer regs replaced by
  the global busy vector).

E2. Control-BO timestamp audit (no build if the BO is already dumped).
- For one sysmem submit, dump Mesa's control/seqno BO and check whether the
  RB_DONE_TS / PC_CCU_FLUSH_COLOR_TS / PC_CCU_FLUSH_DEPTH_TS sequence
  numbers landed (same mapping family as memptrs->fence, which provably
  works). Present + RT=0 → wrote elsewhere (E3). Absent → handshake never
  completed (drain-stall / never-started). Pairs with E1 to give the full
  2x2 (busy × flushed).

E3. Wrong-BO pixel sweep through trusted mappings (harness-only).
- After a solid-colour sysmem blit, scan every userspace-mappable submitted
  BO for the clear pattern (per-component dwords AND swapped variants per
  fd6_color_swap; corpus already learned this lesson). A hit outside the RT
  = addressing class (offsets/aliasing), and its BO identity + offset give
  the stride/alias arithmetic directly. A clean miss everywhere + E1 pulses
  = data never left CCU/GMEM (drain class).

E4. SQE fw swap to stock (file swap + reboot, no kernel build).
- Replace /lib/firmware/qcom/a630_sqe.fw (md5 42b0a156…, 32304 B) with
  current linux-firmware master (md5 9f2540d7…, 34188 B). Re-run the sysmem
  clear. Positive → fw age was the GROUP_ID/mode (or other) skew; then
  bisect. Negative → fw age eliminated as a variable going forward.
- Companion (host-side, no device): qrisc-disasm both blobs, locate SQE PCs
  0x112/0x12e (idle), 0xd00 (draw), 0xeb (LOAD_STATE6_FRAG); audit the draw
  routine for mode/visibility early-exits and the fragment-execution loop.
  The trace PCs are only useful if mapped to handler logic — today they are
  just numbers.

E5. CCU1/SP1 clock-reg readback (one MMIO read each, any live session).
- REG offsets per mesa-a6xx.xml: RBBM_CLOCK_CNTL_CCU1 0xf9, SP1 0xb1 (and
  CNTL2_ 0xb5). deadbead = unimplemented on this SKU (consistent with CAF
  programming only index-0; close the item). 0x0 = implemented but never
  programmed (then program per CAF a640 table and re-run blit).

E6. KGSL a640 init diff (host-side source work, no device risk).
- CHECKS-HWINIT-DRAW.md item 4, still untried and still the best answer to
  "faithful to CAF-msm but CAF-msm never validated on this board": diff the
  tree's hw_init register set against an Android KGSL a640 (SM8150) init
  sequence, restricted to RB/CCU/A2D/UCHE/GMEM_RANGE/NC_MODE/ECO_CNTL +
  PC_MODE_CNTL/RB_MODE_CONTROL (the two mode regs whose post-init state was
  never verified anywhere in the corpus). Do not boot-test candidates
  blindly; the E1 signature first says which block to look at.

E7 (LAST RESORT). Secure-mode/TZ aperture test (PLAN rank 1).
- Only if E1 shows zero busy bits with correct stream + clocks + fw: emit
  CP_SET_SECURE_MODE 0 post-zap (mainline behaviour) instead of relying on
  the direct RBBM_SECVID_TRUST_CNTL write, and re-run. Rationale documented
  in PLAN-GPU-ZERO-TO-FULL.md §5.1; no new reasoning here.

Explicitly NOT recommended: hand-built PM4 oracle (standing rule + would
execute against the same uncharacterised backend — negative result still
ambiguous); any further CPU MMIO of banked clusters; any CP_REG_TO_MEM
variant until its packet encoding is validated against a known-good stream
(266 hard-reset shows the emission itself is faulty); re-litigating UBWC,
tiling, pitch, viewport, relator identity (closed by field decode).

## Ledger — VERIFIED / BELIEVED / UNVERIFIED

VERIFIED (primary source in hand):
- a6xx draw-state/GROUP_ID/visibility/mode-tag/pipelining semantics —
  docs.mesa3d.org/drivers/freedreno.html (fetched 2026-10-16).
- CCU role/carveout/RB_CCU_CNTL fields — same doc + mesa-a6xx.xml @0x8e07.
- Event numbers (CACHE_FLUSH_TS=4, RB_DONE_TS=22, CCU events 24/25/28/29,
  BLIT=30, CACHE_INVALIDATE=49) — envy-pm4.xml.
- CP_SET_DRAW_STATE bit layout (COUNT/DIRTY/DISABLE/DISABLE_ALL/LOAD_IMMED/
  BINNING/GMEM/SYSMEM/GROUP_ID) — mesa-pm4.xml; Mesa usage — fd6_emit.h/c.
- Sysmem/GMEM stream content (SKIP_IB2, VISIBILITY_OVERRIDE, CCU_CNTL,
  RM6_BYPASS/GMEM/BINNING/RESOLVE markers, blit bracket, flush tails) —
  fd6_gmem.c / fd6_blitter.c / fd6_draw.c / fd6_emit.h (local Mesa).
- 1-bin targets take no conditional/visibility path — fd6_gmem.c:247,
  1271, 1434, 1473.
- Local SQE fw bytes + gate arithmetic (buf0=0) — direct read of three
  local copies.
- Stock fw differs (34188 B, md5 9f2540d7) yet shares buf0=0 — fetched blob.
- No a640_sqe.fw upstream; A640→a630_sqe.fw mapping correct — qcom/ listing.
- A630 vs A640 gens/magics/sp/ccu — freedreno_devices.py.
- All-12 ADDR_MODE_CNTL written =1 in current tree; no A2D variant exists —
  a6xx_gpu.c:2404+ / mesa-a6xx.xml.
- RBBM_STATUS bit positions incl. RB/CCU/A2D busy — mesa-a6xx.xml @0x210.
- SCMD appears nowhere (corpus + both XMLs).

BELIEVED (evidence-backed, single-source or indirect):
- Userspace dmabuf RT readback (zeros) is trustworthy — BUFFER-IDENTITY-
  PROVEN + 258 A/B (two cache modes, fence signals on the buffer).
- WFI-passes-while-never-started is the best-fit single mechanism —
  V3 stream analysis + wedge datum; direct proof awaits E1.
- Tree's RBBM_CLOCK_CNTL value correct for A640 — CAF selector quoted
  in-tree + upstream else-branch agreement (two sources, both secondhand).

UNVERIFIED (do not act as if known):
- Which gate RBBM_CLOCK_CNTL bit 7 controls (no bitfields anywhere).
- Whether CCU1/SP1 clock regs are implemented (one read settles it — E5).
- Whether GROUP_ID fragments execute at CP_DRAW on this fw (E4).
- What 0xd00/0xeb firmware routines actually do (needs qrisc-disasm — E4).
- Secure-mode/TZ aperture state (E7 only).
- (REMOVED: UCHE_CLIENT_PF resolved benign — PERFSEL field, Mesa rewrites in-stream.)
- Upstream `a6xx_ucode_check_version` real rejection behaviour (the quoted
  gate fails stock fw too — the quote is at best incomplete; do not cite it
  as evidence of anything until checked against actual upstream source).

## Process notes for the next reader

- The corpus's discipline (continuous + durable + parsing instruments only;
  field-decode against real XML, never recall) is what made every step
  above possible; the two independent catches this session (fw staleness
  with gate refutation; no-a640_sqe upstream) both came from fetching the
  primary artifact instead of trusting a quote.
- Token-budget rule from the assignment was honoured: this file was
  appended incrementally (4 chunks); everything above stands without this
  conversation.
