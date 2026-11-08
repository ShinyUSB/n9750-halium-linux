# CONSULT (experiment/strategy angle) — getting off dead center

Date: 2026-10-16 (env). Corpus labelled "2026-11-16" — same recent work.
Deliverable of the "decisive experiments & strategy" consult. Complements
`consult/silicon-RB.md` and `consult/musespark-SILICON.md` (mechanism angle).

Read before acting on this file:
`STATE-END-OF-SESSION-2026-11-16.md`, `FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md`,
`FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md`, this file.

## 0. What I verified myself (not taken on trust)

- Active instrumented tree: `/home/usb/ShinyKernel-gpu-exp/Kernel` (has
  `ibtrace`, `a6xx_trace_burst`, cpstate node, image 261-266 code).
- Vendor KGSL source present at `/home/usb/ShinyKernel/Kernel/drivers/gpu/msm`
  and contains a **vendor-proven recipe for reading context-banked a6xx
  cluster registers from the CPU** (`adreno_a6xx_snapshot.c:996-1001`).
  This is the headline finding of this consult — see §2 / experiment X1.
- `MSM_INFO_GET_IOVA` is real in this tree: `msm_drv.c` ioctl table
  (`MSM_GEM_INFO`, ~line 1715) → `msm_ioctl_gem_info` handles `MSM_INFO_IOVA`
  at `msm_drv.c:1237`; Mesa's `msm_bo.c:117` uses it. (Replay oracle is
  feasible; verdict in §4 — decline, with reason.)
- ftrace is compiled out on the running kernel (`RESUME.md`: `available_tracers`
  = `nop`, filter permission-denied). Tracepoint/perf plans are dead without a
  rebuild — do not propose them.
- debugfs `rd` unusable — confirmed in-corpus (`FINDING-CP-CONSUMES-RING-RB-
  WRITES-NOTHING.md` §2, note at `msm_drv.c:1220-1222`).

## 1. Reframing the question (the RB framing may be too narrow)

The corpus's own data says the failure is not RB-specific:
`IMAGE-245-RESULTS.md` §3 T1 — a **compute→SSBO write also produces nothing**
(GL err 0, all zero) on a fully powered GPU, and `IMAGE-246-ZAP-LOADED.md` §4
shows it survives the zap. Compute bypasses the RB entirely. So the live
statement is:

> **No shader-domain write path (RB render stores, SP compute stores) has ever
> produced a DDR write, from any source (Mesa render, Mesa compute), with the
> CP domain demonstrably writing DDR.**

Caveat honestly: the compute test predates the Oct-15/16 submit-path fixes
(dangling-IOVA retraction, submission-proven). It should be **re-run once** on
image 263 before being load-bearing again — it is a 10-minute userspace test
(`wrtest.py` T1) with zero device risk. If compute now writes, the problem
narrows to the RB and the whole framing shifts back.

This matters for experiment design: an experiment that only discriminates
"RB broken" vs "Mesa RB state dead" can still be answered wrongly if the true
defect is common to SP and RB (state delivery, aperture/banking, UCHE flush,
secure gate). Any decisive plan must include a compute datapoint.

## 2. HEADLINE FINDING — banked-cluster CPU reads are possible after all

`STATE-END-OF-SESSION` §"why reading the render backend kept failing" says CPU
MMIO of a6xx clusters is unusable due to context banking (method 263). That is
true only for the *default* bus-mux configuration. **The vendor KGSL driver in
this very repo contains the fix**, and it is vendor-proven on exactly this
silicon family:

```c
/* /home/usb/ShinyKernel/Kernel/drivers/gpu/msm/adreno_a6xx_snapshot.c:1000 */
aperture_cntl = ((cur_cluster->id & 0x7) << 8) | (ctxt << 4) | ctxt;
kgsl_regwrite(device, A6XX_CP_APERTURE_CNTL_HOST, aperture_cntl);
/* then plain CPU regreads of the cluster's register ranges land on that
   cluster+context. A6XX_NUM_CTXTS = 2 (same file, line 23). */
```

- `CP_APERTURE_CNTL_HOST = 0x0A00` (also `_SQE` 0x0A01, `_CD` 0x0A03) exists in
  this tree's own `a6xx.xml.h:498` and in current upstream `kern-a6xx.xml:457`
  (where it is typed `PIPE[12:13] CLUSTER[8:10] CONTEXT[4:5]`). **Our driver
  never writes it.** Upstream msm never writes it either — verified by
  full-text search of upstream v6.12 `a6xx_gpu.c` and `adreno_gpu.c` (§10):
  upstream's own crash-state register collection (`adreno_gpu_state_get`)
  uses plain `gpu_read()` with no aperture switch at all, i.e. upstream
  never solved context-banked CPU reads either. KGSL does.
- Cluster IDs (KGSL `adreno_a6xx.h:19-24`): FE=0 SP_VS=1 PC_VS=2 **GRAS=3**
  SP_PS=4 **PS=5**. GRAS cluster covers 0x8000-0x840B (incl. `GRAS_2D_DST_TL/
  BR` 0x8405/6, `GRAS_SC_VIEWPORT_SCISSOR_*`); PS cluster covers 0x8800-0x8C33
  (incl. `RB_MRT_*` 0x8822+, `RB_2D_*` 0x8C00+, `RB_DBG_ECO_CNTL`), with two
  sub-blocks selected by `RB_RB_SUB_BLOCK_SEL_CNTL_HOST` (0x8E3B, value 0=RAC,
  9=RBP — same file lines 80-93). SP/TP/HLSQ clusters need the heavier
  "dbgahb" path (statetype + `HLSQ_DBG_AHB_READ_APERTURE`); defer those.
- Reference values on a known-good system (Mesa CI reference crash dump,
  `/tmp/opencode/mesafull/src/freedreno/.gitlab-ci/reference/crash.log:818-821`):
  `CP_APERTURE_CNTL_HOST: 0`, `0xa01: 0`, `CP_APERTURE_CNTL_CD: 0`.

Two consequences:

1. **The "state never becomes live" question (assignment (a)) becomes directly
   answerable** by reading `GRAS_2D_DST_BR` etc. from the correct
   cluster/context after a submit — no replay, no hand-built PM4.
2. **Image 263's "CPU readback unusable" evidence is weaker than stated.** The
   cited example — "`GRAS_2D_DST_TL` read 0 yet the stream writes it" — is not
   evidence of anything: the stream writes **0** to TL for a 64x64 rect at
   (0,0) (`FINDING-MESA-EXONERATED...` §1 table). The informative register is
   `GRAS_2D_DST_BR` = 0x003f003f, and its value was never reported. Also
   `PERFCTR_GPU_BUSY_MASKED` stuck at 0x002f7fff while ALWAYS_ON advanced may
   be a selector problem (RBBM perf counters need `*_SELECT` programming), not
   proof of anything about idleness.

Also new on the desk (same file family, for the record):
- `CP_REG_TO_MEM` packet fields differ between header generations: the tree's
  generated `adreno_pm4.xml.h` has REG[15:0]/CNT[19:28] (what image 264 coded),
  while current Mesa `mesa-pm4.xml:1080-1086` has REG[17:0]/CNT[29:18]. The
  **vendor KGSL working emission** (`adreno_ringbuffer.c:913`,
  `_get_alwayson_counter`) for a6xx is exactly:
  `[type7 hdr CP_REG_TO_MEM size=3] [reg | (1<<30) | (2<<18)] [addr_lo] [addr_hi]`
  i.e. 64B-mode, CNT=2, 4 dwords total. Image 266's emission should be
  desk-diffed against this before anyone concludes CP_REG_TO_MEM "doesn't
  work" on this chip. Note the vendor only ever uses it on **global** regs
  (RBBM_ALWAYSON_COUNTER); cluster-reg support via this path is unproven
  anywhere. (Encoding question since closed — see §10 item 2: upstream uses
  the identical packet, so images 264-266 failed for other reasons.)
  The aperture route (§2) is the vendor-proven one for clusters.

## 3. Experiment set, ranked by (decisiveness × cheapness) / risk

### X1 — Aperture-correct cluster readback ("did the state become live?")
**THE decisive, minimal experiment. Answers assignment (a) head-on.**

Kernel change (one rebuild), confined to the existing `a6xx_gpu_cpstate`
sysfs node or a new param-gated dump — **zero change to the submit path**:

1. Read raw `0x0A00/0x0A01/0x0A03` and log them (compare vs reference 0/0/0).
2. For cluster in {GRAS(3), PS(5)} × ctxt in {0,1}:
   `gpu_write(CP_APERTURE_CNTL_HOST, (cluster<<8)|(ctxt<<4)|ctxt)`;
   for PS also try sub-block sel 0 and 9 (write `RB_RB_SUB_BLOCK_SEL_CNTL_HOST`);
   read the KGSL cluster register ranges (GRAS: 0x8000-0x8006, 0x8010-0x8092,
   0x8094-0x809D, 0x80A0-0x80A6, 0x80AF-0x80F1, 0x8100-0x8107, 0x8109, 0x8110,
   0x8400-0x840B; PS: `a6xx_ps_cluster_rac/rbp` tables, same file lines 33-47).
3. Restore `CP_APERTURE_CNTL_HOST = 0` (the known-good default) afterwards.

Run: pin PM, load GPU, run the existing clear+draw harness once with
`ibtrace` on, then trigger the dump; also trigger it with NO workload (idle
baseline) so the two dumps diff.

**Ground truth built into the test:** the submitted stream demonstrably
contains `GRAS_2D_DST_BR = 0x003f003f` (`FINDING-MESA-EXONERATED...` §1) and
`RB_MRT_BASE_LO/HI = 0x700025000` (same, §2). If the state became live, one of
the four cluster×context views MUST show it. The idle-baseline diff prevents
"confident wrong answer" #12: a value that is always there proves nothing.

Outcome mapping is in §6. Cost: ~50 lines mirroring vendor code, one flash.
Risk: low — the register is a read-mux; KGSL writes it on healthy systems
during every snapshot; we write it only from the on-demand dump path and
restore it. This is the smallest instrument that can produce a *positive*
observation (state visible) as well as a negative.

**This also answers (b)** — instrumentation inside the pc=0xd00 window: the
existing change-driven tracer (`a6xx_trace_burst`) already triggers on
(pc,pkt) transitions and already catches the CP_DRAW window. Extend it: on a
transition where `pkt == 0x70388003`, set the aperture (GRAS ctxt0/ctxt1) and
read `GRAS_2D_DST_BR`, `GRAS_SC_VIEWPORT_SCISSOR_TL/BR_0`, then restore.
Because the state writes precede the CP_DRAW in stream order, the values must
already be visible at the first draw catch. ~10 extra MMIO per transition;
the burst already tolerates that (it reads 12+ regs per transition today).
Getting data out is then the ordinary `pr_info` TRC path to `dmesg -W > file`.
If the full-dump-per-transition is too slow, log the 3-register mini-set only.

### X2 — Turnip (Vulkan) as an independent cmdstream control (userspace only)
**Cheapest experiment with a real fork in its outcome. Zero device risk.**

Debian 12 ships `mesa-vulkan-drivers` with Turnip (freedreno Vulkan, a640
supported). Turnip builds its cmdstream from scratch — no fd6, no
fd6_blitter/gmem code — through the same kernel. Install on device (apt or
host-side `apt-get download` + `dpkg -i`), run a trivial
`VK_KHR_swapchain`-less clear into a host-visible buffer + `vkQueueWaitIdle` +
map-read (or `vkcube` if a surface works). Also a compute queue write.

- Turnip writes → the kernel+hardware render path works and fd6/Mesa-GL is
  the defect (would be surprising, but then the whole corpus's "backend dead"
  reading is re-framed and the fix is userspace-side).
- Turnip silent → an *independent* Mesa frontend fails identically; "it's
  below Mesa" is confirmed with the strongest cheap control available.
Cost: hours, no rebuild, no flash. Risk: none beyond a normal userspace crash.
Note the standing hazard: gbm/EGL window-surface paths wedged the device
before (`RESULTS-SESSION-258` §1c) — keep PM pinned, run detached, poller up.

### X3 — Re-run compute (wrtest.py T1) on image 263 (userspace only)
10 minutes, zero risk. The compute datapoint predates the submit-path fixes;
the RB-vs-shader-domain framing in §1 depends on it. Do this before X1's
build even finishes.

### X4 — Where did the write GO? (only if X1 shows state live)
If X1 shows the RB armed with a correct live target and still no output, the
remaining fork is "writes nothing" vs "writes somewhere else". The RT IOVA is
0x700025000; a truncation-style misaddress would land in low physical memory
and likely crash the box (it doesn't), so the interesting case is a
wrong-but-mapped address. Sound version, no new PM4: at the next submit of the
same RT BO, the kernel already reads BOs via its own vmap at submit time —
extend that read to run **after the fence lands** (post-retire hook in
`a6xx_retire()` or the burst's early-exit path) and report nonzero dwords of
the RT BO. Kernel-written→kernel-read via the same mapping is the sound
direction (the unsound direction was userspace-cacheable-write → kernel-WC-read,
`CORRECTION-CACHE-COHERENCY.md`; GPU writes bypass CPU caches, so a WC vmap
read after GPU execution is sound for detecting them). Cost: small kernel
edit, one flash. Only build this if X1 forces it.

### X5 — CP_SET_SECURE_MODE 0 from the ring (kernel, conditional)
The mainline packet (`a5xx_gpu.c:587` pattern; opcode defined in our
`a6xx_pm4` header per `IMAGE-134-SECVID-BUILD.md`) has **never been emitted**
on this device — only the direct `RBBM_SECVID_TRUST_CNTL=0` register write was,
and zap-loading alone was shown insufficient (`IMAGE-246`). Upstream v6.12
(`a6xx_gpu.c:1166`) emits `CP_SET_SECURE_MODE 0` from the ring **only after a
successful zap-shader load** and skips it (with a warning) when the zap fw is
absent (`-ENODEV`) — so a faithful replication may need `a640_zap` loaded
first; `IMAGE-243-RESULTS.md` records that the a640 zap fw is not in the
vendor `/firmware` partition and is most likely in the modem filesystem
(`modemst1/2`), which has not been pulled yet — that pull is a prerequisite
sub-task if the bare packet proves insufficient. The CP-works /
shader-silent partition is exactly what a trust gate looks like, and the
corpus's counter-argument (fence writes work) does not refute it, because the
gate could be per-domain. One build, tiny diff, gated behind a module param so
the same image can A/B it. Only fire this if X1 shows "state NOT live" and
X2 also fails — i.e. after cheap options are exhausted. (This is a kernel
edit; it is listed last-on-purpose among kernel options because X1 already
tests the same territory more informatively.)

### X6 — Strategic control: known-good stack on this silicon
Two strengths, two costs:
- **Stock Android boot** (`fastboot boot` the stock boot.img; the vendor image
  was already extracted — `RUNTIME-RESULTS-71-72.md`). Any GLES app renders.
  Confirms hardware on this exact unit (near-zero doubt already) — low value
  per hour, skip unless free.
- **Mainline-kernel spike** (`PLAN-GPU-ZERO-TO-FULL.md` §7, Bentlybro
  gts6l-mainline docs/GPU.md — SM8150+a640+freedreno proven on mainline with
  the same Mesa). This is the real hedge: if X1/X2 land on "backend
  non-functional, no corpus lead explains it", the documented Phase-4 pivot
  (Project Aloha UEFI port + sm8150 mainline + kmsro) converts months of
  CAF-fork archaeology into one known-good driver. Keep it warm, don't start
  it yet.

### X? — Replay-of-Mesa-bytes oracle: DECLINED (for now)
Honest evaluation, since the assignment asked:
- It is buildable (`MSM_INFO_GET_IOVA` verified above; raw submit path
  `rawdrm.py`/`fullpath.py` proven; capture tooling `drawpkts.py`/`rtiova.py`
  exist).
- But its decisive value is subsumed by X1. The replay re-submits bytes that
  the hardware has **already been handed and provably walked**
  (`FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md`). Its only new content would be
  "delivered by our ioctl instead of Mesa's" — and delivery completeness was
  already proven byte-wise (`FINDING-SUBMISSION-IS-COMPLETE.md`: 13/13
  relocations inside submitted BOs, state present, N draws scale). A negative
  result stays ambiguous (as `STATE-END-OF-SESSION` concedes); a positive
  result would most likely mean "Mesa's bookkeeping differs" — which X1 tests
  more directly by observing whether the state became live.
- Cost is the real problem: it is a new subsystem (copy N BOs, patch RT IOVA,
  rewrite nested IB pointers/sizes in an *address-dependent* way — the nested
  graph makes naive patching wrong, because IB dwords embed IOVAs at specific
  offsets), on a project that just spent three device-breaking builds on
  instrumentation. Estimated 2-3 days with several new failure modes, versus
  ~1 day for X1 with none.
- **Revisit only if X1 shows state live AND X2 fails AND X5 fails** — i.e. if
  we end up needing a from-scratch userspace submission path anyway.

## 4. Things already tested/refuted — do NOT re-propose (with citations)

| idea | verdict | corpus |
|---|---|---|
| UBWC / tiling / binning / GMEM vs sysmem matrix | all fail identically | `STATE-SUBMIT-PROVEN-RENDER-DEAD.md` §2 |
| cache mode (WC vs cached) | A/B'd, no change | `RESULTS-SESSION-258.md` §1a |
| CPU↔GPU coherency as cause | killed (all BOs are WC; demotion logic) | `PLAN-GPU-ZERO-TO-FULL.md` §2.3 |
| LLC/SCID, amsbc, A640 clock values, ROQ2, GMU AO CGC, hwcg table/TEX_FCHE, QSB, RBBM_NC | tested, refuted | `IMAGE-244-RESULTS.md` cumulative table |
| debugfs `rd` register reads | unusable in this tree | `FINDING-CP-CONSUMES...` §2 |
| ftrace / tracepoints / perf | not compiled in | `RESUME.md` |
| hand-built PM4 | forbidden by standing rule (2 fabricated claims) | `STATE-END-OF-SESSION` |
| CP_REG_TO_MEM variants 264/265/266 | deadlock/deadlock/hard-reset; never successfully run; encoding now desk-diffable vs vendor (§2) | `STATE-END-OF-SESSION` §264-266 |
| CP_SMMU_TABLE_UPDATE | retracted (v5.10+ feature) | `IMAGE-244` table |
| "shaders never submitted" / dangling IOVA | retracted (containment check 13/13) | `FINDING-SUBMISSION-IS-COMPLETE.md` §3 |
| "SQE never starts" / frozen at 0x12e | retracted (idle loop) | `STATE-STREAM-NEVER-RETIRES.md` retraction 1 |
| SQE ucode version-gate as root cause | demote: the ucode provably walks the graph and dispatches CP_DRAW; a gate-failing-but-working microcode is no longer a credible sole cause | `FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md` |
| glReadPixels as evidence | unsound when RB dead (GPU blit) | `MEASUREMENT-VALIDITY-AUDIT.md` |
| BO identity by size inference | retired (cost 19 images) | `ROOT-CAUSE-SHADERS-NEVER-SUBMITTED.md` |
| `gbm_bo_get_fd`-based prefill oracles | impossible on this tree (no dma_buf_ops) | `RESULTS-SESSION-258.md` §1c |
| CP_ME_INIT mask divergence | **downgraded to low**: verified upstream v6.12 `a6xx_cp_init` emits `0x2f,0x03,0x20000000,0,0,0,0,0` — byte-identical to ours (`a6xx_gpu.c:2116-2139`, matches `RETRACTION-SMMU-TABLE-UPDATE.md:107`); mainline a6xx renders fine on SDM845-class parts with exactly this init and no `pwrup_reglist`. The divergence is CAF-vs-upstream, not ours-vs-working-baseline. (The "0x150 in dword 5" figure circulating in session notes occurs in neither tree — transcription error, ignore it.) | upstream `a6xx_gpu.c:616-634` + `IMAGE-244` cumulative + `RETRACTION-SMMU-TABLE-UPDATE.md:107` |

## 5. Decision tree (result X → go to Y)

```
X3 (compute re-run, 10 min, userspace)
 ├─ compute NOW writes  → the shader domain is alive; RB-specific framing returns.
 │                        Go straight to X1 (RB cluster readback) with focus on
 │                        GRAS/PS only; the SSBO path is a different bug or was
 │                        fixed by the Oct-15/16 submit fixes — document and move.
 └─ compute silent (expected) → §1 framing stands; continue.

X1 (aperture readback)
 ├─ 0x0A00/01/03 != 0/0/0 (reference values) → bring-up left the bus mux wrong.
 │   That is a specific, cheap fix (write the reference value at hw_init) and
 │   likely THE class of bug. Fix, re-run harness, celebrate or continue.
 ├─ GRAS_2D_DST_BR reads 0x003f003f in some (cluster,ctxt) → STATE IS LIVE.
 │   "Mesa state never becomes live" is DEAD. The RB is armed with a correct
 │   target and writes nothing. Then:
 │   ├─ run X2 (Turnip) for the independent-control datapoint;
 │   ├─ if X2 silent too → the write stage itself is dead. Candidates in order:
 │   │   X5 (secure-mode packet), X4 (misdirected-write hunt), UCHE/CCU flush
 │   │   semantics (`PLAN-GPU-ZERO-TO-FULL` Phase B2 #3), CP_ME_INIT pwrup_reglist
 │   │   (`PLAN-GPU-ZERO-TO-FULL` Phase B2 #2 — CAF's list is exactly the
 │   │   per-block ADDR_MODE/UCHE/RB_NC/GMEM_RANGE set a shader-domain write
 │   │   needs; it has never been ported).
 │   └─ if those exhaust without a hit → declare "not answerable with this
 │       stack" and pivot to X6-mainline with the CAF tree kept as reference.
 └─ BR/MRT reads show defaults in ALL (cluster,ctxt) views while the stream
     demonstrably wrote them → the SQE's cluster writes are being routed
     (aperture/SQE-side context) somewhere the host cannot see, or dropped.
     ├─ check 0x0A01 (SQE aperture) against reference — if wrong, fix bring-up.
     ├─ retry CP_REG_TO_MEM for ONE register (GRAS_2D_DST_BR) using the vendor
     │   emission exactly (§2), desk-diffed twice before flash. One packet,
     │   one register — not a capture subsystem.
     └─ if that also fails → register observation is closed; fall to X2/X6.
```

## 6. What I would NOT do (plainly)

1. **Not build the replay oracle now** — subsumed by X1, ambiguous-negative,
   2-3 days, new failure modes (§3 X?).
2. **Not attempt another CP_REG_TO_MEM capture subsystem** — three failures
   already; retry only as the single-register, vendor-exact variant inside the
   tree above, and only if X1 forces it.
3. **Not sweep more hw_init register divergences** — the CAF/mainline-attested
   list is exhausted and refuted (`IMAGE-244` table). Any new kernel-side
   hypothesis must first earn a discriminating observation from X1/X2.
4. **Not read banked cluster registers without the aperture** — image 263
   already burned on this; and note its cited evidence was TL=0 (which is the
   correct value!). Re-analyze any existing TRC logs for `gras_br` before
   rebuilding anything — if image 263 traces exist on the device
   (`/root/trc-*.txt`), they may already contain `gras_br=0x003f003f`, which
   would make X1's rebuild unnecessary. **Check the device first.**
5. **Not run sysmem configurations without file capture + poller** — that
   config has frozen the device (`FINDING-SYSMEM-WEDGES-RB-PATH.md`).
6. **Not chase the SQE ucode version gate** as a root cause — the walk tracer
   proved the ucode executes the graph; the gate is a hygiene issue at most.
7. **Not flash anything that changes the submit path** until X1's read-only
   instrument has run. The next build's diff must be reviewable as
   "instrumentation only" — that is what makes its result believable after
   eleven wrong instruments.

## 7. Sequencing (what to do tomorrow morning, in order)

0. On-device, no rebuild: check `/root/trc-*.txt` for `gras_br=` values from
   image 263/266 runs (may already answer X1's question). 15 min.
1. X3: re-run `wrtest.py` T1 (compute) on the current image. 10 min.
2. X2: install Turnip, run Vulkan clear + compute write, detached with poller.
   Hours, zero flash risk.
3. Build X1 (aperture dump in the cpstate node + mini-set in the burst).
   One flash. Run idle-baseline + workload dumps.
4. Follow §5 tree.

## 8. LIVE DEVICE ADDENDUM (appended after desk work; phone was reachable)

SSH path: `ssh -i /home/usb/artifacts/keys/d2q_rsa root@10.15.19.82` over the
USB RNDIS gadget (`/home/usb/.opencode-tmp/u.sh` manages the link). Device
healthy at time of check: `SPTPRAC 0x0`, `RPMH_POWER_STATE 0x0`,
`CP_TIMESTAMP clocking 1` with advancing delta, no D-state tasks. Current
kernel = image 263 build (has `ibtrace`, `ibdump`, `pm_hold`, `reglog`,
`rd_full`; does NOT have `ibcap` — matching `STATE-END-OF-SESSION`).
`ibtrace=0`, `ibdump=N` at rest. Note: loadavg ~11 with no CPU consumer —
cosmetic, not a wedge.

**Step 0 of §7 is DONE, and its result strengthens X1.** Old walk traces are
on the device (`/root/walk-rb.txt`, image 262 RB-tracer run). Its three
transition lines show, for the standard 64x64 clear+draw workload whose stream
demonstrably writes `GRAS_2D_DST_BR = 0x003f003f`
(`FINDING-MESA-EXONERATED...` §1):

```
TRC[seq=1 i=001] pc=0xeb pkt=0x70348003 ... gras_tl=0x00000000 gras_br=0x00000000 rb_mode=0x00000001
TRC[seq=1] walk_end ... gpubusy_delta=0 alwayson_delta=6724 gras_tl_changed=0
```

So, with the correct register this time (BR, not TL — TL=0 is the value the
stream writes and proved nothing): **the default-context CPU read does not see
the CP's write to the GRAS cluster.** That is now a measured fact on the right
register, and the aperture sweep (X1) is exactly the experiment that
discriminates "written to a context the host cannot see" from "never written".
Two more observations from the same file:

- `rb_mode = RB_ADDR_MODE_CNTL = 0x00000001` (= ADDR_64B, the same value the
  known-good reference dump shows for the analogous *_ADDR_MODE_CNTL regs) —
  not a smoking gun, but it is a banked-cluster (PS) register that reads a
  *sane* value, so PS-cluster host reads are not wholesale dead.
- `gpubusy = 0x002f7fff` constant — as suspected, `PERFCTR_GPU_BUSY_MASKED`
  without `*_SELECT` programming reads a fixed value; the trace's
  `gpubusy_delta=0` is an unprogrammed-counter artifact, NOT evidence of an
  idle GPU. Retire that reading if it is cited anywhere.

Also discovered in the live build (may already exist in tree builds):
`msm.reglog` module param (0600) logs every `gpu_read`/`gpu_write` — useful to
timestamp the aperture writes of X1 against the TRC stream; and `rd_full`
(msm_rd.c snapshot of all buffer contents) — orthogonal, note only.

Practical consequence for sequencing: §7 step 0 answered (see above); X3
(compute re-run) and X2 (Turnip) can both run against this live device today
with zero flash risk; X1 is the one build needed.

## 9. Honesty notes

- Device access during this consult was **read-only**: SSH over RNDIS to list
  files, `cat` kernel params / sysfs nodes / `/root/walk-rb.txt`, and health
  checks. No GPU workload was triggered, no file was written, nothing was
  flashed. Everything else is desk verification against the trees and corpus
  on this host.
- X1's one assumption I cannot verify from here: that the CP's own writes to
  cluster regs land in a context the host aperture can select (KGSL snapshots
  capture *preempted render contexts*, which is the same situation, so the
  assumption is vendor-corroborated but not proven for our exact config).
  The idle-baseline + ground-truth design makes the failure mode of this
  assumption visible rather than silent.
- If X1, X2, X3 all come back with the backend dead and no lead survives, the
  honest conclusion is that this specific CAF 4.14 fork's a6xx bring-up has a
  defect no corpus instrument can see, and the right answer is the mainline
  pivot (X6), not another register sweep. That is a legitimate outcome and
  should be pre-agreed now, not negotiated after image 300.

## 10. Upstream (Linux v6.12) verification — appended 2026-10-16

Fetched and full-text-searched the two relevant upstream files
(`drivers/gpu/drm/msm/adreno/a6xx_gpu.c`, 2343 lines; `adreno_gpu.c`, full):

1. **No `CP_APERTURE_CNTL_HOST` anywhere in either file** (the only
   aperture mentions in the whole Mesa-side source tree are XML register
   *definitions*, `a6xx.xml:1094`, no writer). Upstream's crash-state
   collection `adreno_gpu_state_get()` reads every register with plain
   `gpu_read()` and no bank/context switch — i.e. upstream silently inherits
   the same context-banking problem this project hit, and never noticed
   because it only matters for snapshot accuracy, not rendering. Two
   consequences: (a) X1's mechanism has **KGSL as its only prior art**, so
   nobody to crib from beyond `adreno_a6xx_snapshot.c` — the plan already
   cites; (b) the "we read 0 from banked clusters" observation has an
   innocent upstream analogue, so it is definitively not evidence of a
   dead block.
2. **`CP_REG_TO_MEM` in upstream `get_alwayson_counter()`** (`a6xx_gpu.c:95-98`)
   is exactly `OUT_PKT7(CP_REG_TO_MEM,3)` + `REG | CNT(2) | 64B` —
   structurally identical to the vendor KGSL emission desk-diffed in §2. The
   packet is spec-conformant and used by both working driver lineages; the
   field-layout worry in §2 only applies to *hand-building* the packet, which
   the standing rule forbids anyway. This closes the §2 CP_REG_TO_MEM
   encoding question (definitively: it is not why images 264-266 failed).
3. **`a6xx_cp_init` upstream == ours, byte-for-byte**: `CP_ME_INIT` size 8,
   dwords `0x2f, 0x03, 0x20000000, 0, 0, 0, 0, 0` (`a6xx_gpu.c:616-634`,
   matches our `a6xx_gpu.c:2116-2139` and `RETRACTION-SMMU-TABLE-UPDATE.md:107`).
   See the updated §4 row: the divergence is CAF-vs-upstream, not
   ours-vs-working-baseline. Mainline a6xx (SDM845 class) renders with this
   exact init and no `pwrup_reglist` on the same silicon family.
4. **`CP_SET_SECURE_MODE 0` upstream** (`a6xx_gpu.c:1166`) is emitted from
   ring 0 only after `a6xx_zap_shader_init()` succeeds, then `flush`+`idle`;
   on `-ENODEV` (no zap in DT) it is skipped with a warning that
   `RBBM_SECVID_TRUST_CNTL` access may then fault. Our tree emits it nowhere
   in a6xx (only `a5xx_gpu.c:587`) — confirming X5's premise from upstream
   source, and adding the design detail now reflected in §3/X5: the packet's
   legitimate context is post-zap-auth, and Samsung's TZ rejecting the
   test-signed `a640_zap` (`IMAGE-243-RESULTS.md`) is the known obstacle to
   following upstream verbatim.
