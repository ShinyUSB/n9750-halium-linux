# State of the Adreno 640 bring-up — after the 2026-10-21 session

## The one thing that changed everything

For 260+ images the project measured only two clients — the **shader (SP/UCHE)**
and the **render block (RB/2D)** — and both never wrote to memory. The **CP
(command processor)** was never used as a control, and it turns out to work.

Measured on image 276, healthy CP, one boot:

```
=== STEP 2: our raw CP_MEM_WRITE, on that same CP ===
    TARGET after A = (286331153, 0, 0, 0)
    A_WROTE=True                       <- 286331153 == 0x11111111
```

and, in the same boot, the workload still writes nothing:

```
  VERDICT: GPU WROTE NOTHING - prefill 0xAA fully intact, oracle-validated
```

| client | write to a userspace buffer |
|---|---|
| **CP** | **WORKS** (0x11111111 landed) |
| SP/UCHE (shader) | fails (0/64 SSBO, 0/6 FBO) |
| RB/2D (render block) | fails (0/4096 CP_BLIT) |

Also proven: the CP **executes userspace command streams**
(`probe_cp_exec.py`: `CP_EVENT_WRITE 0x5A5A1234` into the memptrs' `rptr`
word landed and the driver read it back), and it **returns to the ring**
after an indirect buffer (the driver's fence word advances).

**So the memory path, the MMU, the page tables, the command stream and the
CP are all fine. The defect is inside the 3D domain.**

## Closed negatives (each with its evidence)

| hypothesis | image | evidence |
|---|---|---|
| the GPU is in secure mode (`TRUST_CNTL`) | 277/278 | ZAP loaded and running (`active_count(1)`), `CP_SET_SECURE_MODE(0)` issued by the kernel's `OUT_PKT7` and consumed (RPTR 0x9→0xb), `TRUST_CNTL` unmoved (0x1→0x1) |
| TZ was never told the GPU's SMMU aperture | 278 | `scm_call2(SCM_SVC_MP, CP_SMMU_APERTURE_ID)` with the CB from `DOMAIN_ATTR_CONTEXT_BANK`: `avail=1 ret=0`, TZ accepted it, nothing changed |
| the indirect buffer's size is wrong (bytes vs dwords) | 276 | size = 4, 16, 64, 256, 1024, 4096: the fence never advanced for any |
| the UCHE's address filters gate the writes | 279 | written and read back as CAF's zeros (`RANGE_MAX=0x0 TRAP_BASE=0x0 WRITE_THRU=0x0`); still dead |
| the shader-pipeline `PC_DBG_ECO_CNTL` is wrong | 280 | written and read back as `0x00200200` (upstream's a640 value); still dead |
| the SMMU rejects the write | 276/279/280/281/282 | **zero** SMMU faults in the whole run; the page table maps the target (PTRMAP) |
| the AHB rejects the write | same | the tree already sets `CP_AHB_CNTL=1` (unmapped AHB accesses error) and no AHB error appears |
| CAF's a640 hwcg (clock/power) table | 281 | CAF's table copied verbatim (49 entries, incl. `CLOCK_HYST_TEX_FCHE=0x777` and the three registers we never wrote); still dead |
| the UCHE's GMEM (on-chip memory) window | 282 | programmed with CAF's formula and read back as `[0x100000, 0x13ffff]`; still dead |

## Method corrections (they voided earlier results — worth keeping)

1. **The submit ioctl's "fence retired" is not evidence of GPU execution.** The
   CPU-visible fence word (`ring->memptrs->fence`, written by the GPU) is.
   Images 277/278 were measured on a CP that was fetching but *not executing*
   (`scratch2=0x00000000 fence=0`), so their results were void. Image 276 shows
   `fence=1,2,3,4`. Always print the fence as a precondition.
2. **PM4 type-7 headers need the parity bits**:
   `(7<<28) | cnt | (parity(cnt)<<15) | (opcode&0x7f)<<16 | (parity(opcode)<<23)`.
   The harness now asserts this against the project's own known-good header
   (`CP_DRAW` = `0x70388003`). The old harness omitted them, voiding two runs.
3. **Two packets leave the GPU fetching without executing**: `CP_SET_SECURE_MODE(0)`
   and `CP_EVENT_WRITE` with the `CACHE_FLUSH_TS` event id. Both are powerful
   levers and both look like "the GPU is broken".
4. **`a6xx.uche_filter_clear` / `a6xx.pc_eco_a640` / `a6xx.cp_secure_off`**
   are runtime-selectable; keep exactly one active per image.

## Ranked next candidates (all in the 3D domain)

> **RECOMMENDATION: stop eliminating one register per flash and go get the
> ground truth.** Nine hypotheses have now been closed with clean negatives
> since the localisation was established, each costing a build, a flash and a
> 15-minute run. The Android build has CAF's `snapshot/` directory
> (`dump`, `snapshot_crashdumper`, `force_panic`); `force_panic` produces
> CAF's **text register dump of a WORKING GPU**. That single file would let
> us diff the 3D domain's registers wholesale — by register name, which is the
> method that found the hwcg differences — instead of guessing which
> register to try next. It costs one reboot, and RNDIS now comes up by
> itself.

0. **[DONE — image 281] CAF's a640 hwcg table, verbatim.** A mechanical diff
   of CAF's `a640_hwcg_regs[]` against the project's table — comparing
   register *names*, not readback offsets — found four differences the
   project had never seen (it compared readbacks by offset, which is why a
   value mismatch and three *missing* registers both read as "43/46 exact,
   harmless"):

   | register | CAF | project |
   |---|---|---|
   | `A6XX_RBBM_CLOCK_HYST_TEX_FCHE` | `0x00000777` | `0x00000000` |
   | `A6XX_RBBM_ISDB_CNT` | `0x00000182` | missing |
   | `A6XX_RBBM_RAC_THRESHOLD_CNT` | `0x00000000` | missing |
   | `A6XX_RBBM_SP_HYST_CNT` | `0x00000000` | missing |

   TEX_FCHE is the texture-fetch engine — the block that moves data between
   the GPU and memory — and the project's own note on this table says the
   TEX_FCHE clock gate "was never programmed", with a pipeline whose fetch
   stage is not clocked producing exactly the observed symptom. The table had
   TEX_FCHE's CNTL and DELAY but **zeroed its HYST** where CAF programs
   `0x777`.

   Image 281 uses CAF's 49-entry table; the old 46-entry one is kept in the
   tree and is selectable with `a6xx.hwcg_caf=0` (cmdline-only change).
   **Image 281's cmdline is byte-for-byte image 276's**, so 276 vs 281 differ
   *only* in the kernel payload.

1. **The remaining CAF-vs-ours init in the 3D domain** — the diff of CAF's
   `adreno_a6xx.c` against our `a6xx_gpu.c` for the UCHE/SP/RB/CCU/VSC
   registers. The mechanical pass is noisy (our driver writes many of them
   through the hwcg table), so it must be done *including* that table — and
   it must compare by **register name**, not by offset. That single change
   of method is what found the hwcg differences.
2. **The per-client GBIF configuration for the non-CP clients.** We write
   CAF's `a640_gbif[]` (QSB_SIDE0..3 = `0x00071620`,
   `RBBM_GBIF_CLIENT_QOS_CNTL` = `0x3`) and the project measured "no effect"
   — but that was measured against a symptom that (we now know) is insensitive
   to it. It needs re-testing with a *sensitive* oracle.
3. **The GMU's own memory-path setup** (firmware, not driver): the GMU
   programs the NoC/GBIF attributes at boot.
4. **The GMEM window (new, weak-moderate).** We program
   `UCHE_GMEM_RANGE = [0x100000, 0x100000 + gpu->gmem - 1]` and
   `gpu->gmem` is **0** for a6xx in this tree (the per-chip info has no
   `.gmem`), so the UCHE's on-chip-memory window is the degenerate
   `0x100000..0x0FFFFF`. CAF programs
   `[ALIGN(gmem_size,1M), ALIGN(gmem_size,1M)+gmem_size-1]` with
   `gmem_size = 256K` → `0x100000..0x13FFFF`. If the shader's private
   memory is misrouted out of the GMEM window it would land in the (broken)
   system-memory path, which would explain a shader that cannot run.
   *Caveat:* upstream msm programs our value and works on a640 elsewhere, so
   this is a candidate, not a finding.
5. **Ground truth from the working device — ROUTE FOUND, BLOCKED IN THIS
   KERNEL.** The Android build has CAF's `snapshot/` directory (`dump`,
   `snapshot_crashdumper`, `force_panic`, `snapshot_legacy`) and the flags all
   set (`crashdumper=1 force_panic=1`), but the dump is **always 0 bytes**:
   `adreno_a6xx_snapshot.c:1472-1476` bails out unless
   `a6xx_capturescript.gpuaddr` and `a6xx_crashdump_registers.gpuaddr` are
   non-zero, and **nothing in this tree ever assigns them** (they are only
   read). The a6xx crashdump path is unimplemented here, so `force_panic`
   cannot produce a register dump. There is also no `/dev/mem`
   (CONFIG_DEVMEM off) and no `/proc/kcore`.
   *To get the ground truth: a kernel with the a6xx snapshot capture buffers
   actually allocated (CAF's `a6xx_snapshot_init()` path), or
   CONFIG_DEVMEM/CONFIG_PROC_KCORE, or an ioremap-reading module.*

## Environment repairs made this session (all verified)

* **RNDIS now auto-starts**: `d2q-rndis.service` (enabled, `TimeoutStartSec=120`,
  `ExecStart=/usr/local/sbin/d2q-rndis-ensure`) plus
  `d2q-rndis-keepalive.timer` (30 s). The vendor unit could never start
  (`ConditionPathExists=/sys/kernel/config/usb_gadget` is unsatisfiable on a
  fresh boot) and `/usr/bin/d2q-rndis` was not idempotent (`ip link set …
  address` returns EEXIST when networkd already applied the MAC).
* The Debian rootfs was restored to `/root/userdata/rootfs.img` (md5-verified)
  after the Android data clear wiped it, and `/root` was restored from the
  Oct 20 backup.
* `a640_zap.{mdt,b00,b01,b02}` were restored to `/lib/firmware` (the ZAP load
  had been failing with "Failed to locate a640_zap.mdt").

## Harnesses (all in /home/usb/.opencode, all pushed to the phone)

| file | what it does |
|---|---|
| `secvid_cp.py` | `CP_MEM_WRITE` control (A) and the secure-mode variant (B); correct PM4 parity, self-asserting |
| `probe_cp_exec.py` | asks the CP to write `0x5A5A1234` into the memptrs' `rptr` word from a **userspace** IB — the proof that the CP executes userspace command streams |
| `nib.py`, `nib3.py` | indirect-buffer probes (NOP-only; size sweep) |
| `run276workload.sh`, `run277.sh`, `run278.sh`, `run279.sh`, `run280.sh` | the per-image bring-up + measurement scripts; each prints the fence as a precondition |
| `pkg277.py` … `pkg280.py` | per-image packers (each verifies payload/ramdisk/cmdline/id-field) |

## The ground-truth diff (2026-10-22) — first mechanical working-vs-broken comparison

Method: the WORKING Android/KGSL device's register state was captured via
`/sys/class/kgsl/kgsl-3d0/snapshot/dump` (2342 entries, 662 named, chipid
0x06040001 = a640).  The BROKEN Debian device was read through the driver's own
ioremap (image 283 adds `a6xx_cp_state_regs`, triggered by writing `REGS` to
the cpstate attribute; the offset list is the set CAF reads safely).
Artifacts: `artifacts/gpu-bringup/ground-truth/`.

487 registers comparable, 252 differ, **8 of them configuration**:

| register | broken | working |
|---|---|---|
| `A6XX_UCHE_GMEM_RANGE_MAX_LO` | 0x0013f000 | **0x001ff000** |
| `A6XX_CP_RB_CNTL` | 0x0000020c | 0x0800020c |
| `A6XX_RBBM_INT_0_MASK` | 0x03d0e242 | 0x13d0e3c2 |
| `A6XX_RBBM_INTERFACE_HANG_INT_CNTL` | 0x401fffff | 0x40cfffff |
| `A6XX_CP_CONTEXT_SWITCH_SMMU_INFO_LO` | 0x00000000 | 0xfc496000 (runtime) |
| `A6XX_CP_SQE_CNTL` | 0x00000001 | 0x00000000 |

**The GMEM window is the one with a mechanism.**  We program
`UCHE_GMEM_RANGE = [0x100000, 0x100000 + gpu->gmem - 1]` and `gpu->gmem` is
**0** for a6xx in this tree, so the shader's on-chip-memory window has always
been ~1 MB too small; the working device's window is 2x ours.  Image 282's
"CAF formula" (256 KB) was *smaller still* — CAF's `gmem_size = SZ_256K` is
not the a640's; the a640's is 1 MB (`max = 0x1fffff` reads back as
0x1ff000).  If the shader's private memory sits in the upper part of that
window, it is misrouted into the system-memory path — the path that is broken
for the 3D domain while the CP's works.

**Image 284** = image 283 with `a6xx.gmem_window=1024` (cmdline only, no
rebuild) — the GMEM window of the working device.  The other three
configuration differences are one-value-each candidates: `CP_RB_CNTL` bit 27
(CAF uses `A6XX_CP_RB_CNTL_DEFAULT`, we compose the value ourselves),
`INT_0_MASK` bit 28, `INTERFACE_HANG_INT_CNTL` bit 22.

## ROOT CAUSE FOUND (2026-10-22): our CP_PROTECT table is misaligned

The working-vs-broken register diff (ground-truth/) shows the CPU_PROTECT table
-- the list of register ranges the CP is allowed to write -- shifted by exactly
one slot:

| slot | ours (read back) | working (read back) | decode |
|---|---|---|---|
| 0x85e | 0x80380e00 | 0x80040e00 | ours RW(0x0e00,0x38) / working RW(0x0e00,1) |
| 0x85f | 0x80008e00 | 0x802c0e03 | ours RW(0x8e00,0) / working RW(0x0e03,0x0b) |
| 0x860 | 0x803c8e50 | 0x80008e00 | ours RW(0x8e50,0xf) / working RW(0x8e00,0) |
| 0x861 | 0x8000be02 | 0x803c8e50 | ours RW(0xbe02,0) / working RW(0x8e50,0xf) |

**Cause (a6xx_gpu.c:3042 vs adreno_a6xx.c):** we merged CAF's two entries
into one:

    ours:  CP_PROTECT(14) = A6XX_PROTECT_RW(0xe00, 0xe);      <- ONE range
    CAF:   { 0xE00, 0x1, 1 },                                 <- slot 14
           { 0xE03, 0xB, 1 },                                 <- slot 15

Because we use one slot where CAF uses two, **every protection range from slot
14 onward is offset by one** -- and the ranges that land in the wrong slot
include the 0x8E00 block, which is exactly why on OUR device

    0x8e01..0x8e77  ->  0xdeadbead   (Adreno's "register protected" answer)

while the working device has real values there (e.g.
A6XX_RB_RB_SUB_BLOCK_SEL_CNTL_HOST = 0x9,
A6XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE = 0x1).

**Cause (missing entirely):** CAF's a6xx_protect_init() appends the MMU/SMMU
register range from the DT property

    qcom,protect = <0xa0000 0xc000>;      (sm8150-gpu.dtsi, "CB5(ATOS) & CB5/6/7
                                            are protected by HYP")

as extra read-protected slots (0x2000 per slot, 6 slots).  Our driver never
applies them, so on our device the SMMU register block is left writable to
the GPU -- while on the working device it is protected, as Samsung's
hypervisor requires.

**Next image (285):** program CAF's 28-entry table verbatim (splitting
0xE00/1 and 0xE03/0xB), then append the DT-derived qcom,protect slots
exactly as CAF's a6xx_protect_init() does.

## Image 285: the CP_PROTECT fix is CONFIRMED (but it was not the gate)

The register dump after image 285 (CAF's 28-entry table + the DT's
qcom,protect slots) now matches the WORKING device **slot for slot** --
verified in `ground-truth/diff-285-vs-working.txt`:

    CP_PROTECT slot 14  off=0x0085e  broken=0x80040e00  working=0x80040e00  MATCH
    CP_PROTECT slot 15  off=0x0085f  broken=0x802c0e03  working=0x802c0e03  MATCH
    CP_PROTECT slot 16  off=0x00860  broken=0x80008e00  working=0x80008e00  MATCH
    ... every slot MATCH

So the off-by-one (we had merged CAF's {0xE00,1} and {0xE03,0xB} into
{0xE00,0xE}) is a REAL bug and is now fixed.  The shader's writes are still
dead, and the CP control still works -- so the protection table was a
misconfiguration, not the gate.

**The next candidate, now with hardware evidence: the GPU's CX domain is not
powered on our device.**

| register | ours | working |
|---|---|---|
| `A6XX_GPU_CC_CX_GDSCR` (0x2441b) | 0xf8222800 | **0xf8228000** (bit 23 set) |
| `A6XX_RB_RB_SUB_BLOCK_SEL_CNTL_HOST` (0x8e3b) | 0xdeadbead (unreadable) | 0x00000009 |
| `A6XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE` (0x8e50) | 0x00000000 | 0x00000001 |

The 0x8e00 block reads as *unimplemented* on our device and has real values on
the working one -- consistent with that whole block living in the CX domain
and the domain being off.  The project examined "cx_is_on" early (images
274c/275) and dismissed it ("would be inert: we never call
regulator_disable()") -- the GDSCR readback contradicts that dismissal.

Also still open, each a single value with the target known:
`A6XX_CP_RB_CNTL` bit 27 (0x08000000; CAF uses A6XX_CP_RB_CNTL_DEFAULT),
`A6XX_RBBM_INT_0_MASK` bit 28, `A6XX_RBBM_INTERFACE_HANG_INT_CNTL` bit 22.

## The CX domain, and a caveat about our own comparison

CAF treats the GPU's **two** GDSC domains as one unit
(sm8150-gpu.dtsi: `regulator-names = "vddcx","vdd"`):

  * `kgsl_gmu.c:1534`  regulator_enable(gmu->cx_gdsc)      -- CX on
  * `kgsl_gmu.c:1599`  regulator_set_mode(cx_gdsc, IDLE)    -- on suspend
  * `kgsl_gmu.c:1604`  regulator_set_mode(cx_gdsc, NORMAL)  -- on resume
    and its own comment: "After GX GDSC is off, CX GDSC must be off. Voting
    off alone from the GPU driver cannot guarantee CX GDSC off."

We DO enable it (`msm_gpu.c:73 regulator_enable(gpu->gpu_cx)`), but we never
set its **mode** to NORMAL.  The project tested that in images 274c/275 and
found the secure firmware aborts immediately at `regulator_set_mode` -- so on
this part the mode cannot be set by the driver, and the CX domain stays in
its default (IDLE) mode.  The register readback agrees the domain is in a
different state than the working device's:

    A6XX_GPU_CC_CX_GDSCR   ours=0xf8222800   working=0xf8228000

**CAVEAT -- our comparison is not yet like-for-like.**  The working device's
snapshot was taken with the GPU **actively rendering** (gpubusy was 301554;
the compositor was driving it), while our Debian GPU was **idle**.  So every
remaining difference may be activity-dependent rather than configuration:
the GDSCR bit, `RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE` (0 vs 1) and the
unreadable 0x8e00 block could all be consequences of the working GPU being
busy, not causes of ours being dead.

Two of the remaining differences are definitely **static init-time values**
(programmed at attach, independent of activity) and are therefore real:

  * `A6XX_CP_RB_CNTL`            ours=0x0000020c  working=0x0800020c  (bit 27;
    CAF writes A6XX_CP_RB_CNTL_DEFAULT, we compose the value ourselves)
  * `A6XX_RBBM_INT_0_MASK`       ours=0x03d0e242  working=0x13d0e3c2  (bit 28)
  * `A6XX_RBBM_INTERFACE_HANG_INT_CNTL` ours=0x401fffff working=0x40cfffff (bit 22)

**Image 286** should set all three to the working device's values (they are
init-time constants) -- and, in the same boot, take a *like-for-like* dump of
the working device with its GPU IDLE, so the next diff is apples-to-apples.

---

## Images 286 / 287 (2026-10-22) — init-time register match: NEGATIVE, but now exact

### 286 — `CP_RB_CNTL_NO_UPDATE`: REFUTED by experiment
Image 286 set `AXXX_CP_RB_CNTL_NO_UPDATE` (0x08000000) in the generic ring setup so our
`CP_RB_CNTL` would read back the working device's 0x0800020c.  Result: **the attach hung** —
no `HWINIT[25] return ret=0`, the `echo 1 > a6xx_gpu_load` write blocked forever, i.e. the CP
never became idle.  `NO_UPDATE` means something different on a640 than a430; the working
device reaches 0x0800020c by another route.  A readback value is a *target state*, not a code
recipe.  Reverted (source comment records why).

### 287 — the other two values applied: no effect on rendering
`RBBM_INT_0_MASK = 0x13d0e3c2`, `RBBM_INTERFACE_HANG_INT_CNTL = 0x40cfffff` (working device's
values), `CP_RB_CNTL` left at 0x0000020c.  Cmdline identical to 285; all else unchanged.

Readback (dmesg `REG286:`) and the dump both confirm the writes stuck:
| reg | 285 | 287 | working |
|---|---|---|---|
| `RBBM_INT_0_MASK` (0x038) | 0x03d0e242 | **0x13d0e3c2** | 0x13d0e3c2 |
| `RBBM_INTERFACE_HANG_INT_CNTL` (0x01f) | 0x401fffff | **0x40cfffff** | 0x40cfffff |
| `CP_RB_CNTL` (0x802) | 0x0000020c | 0x0000020c | 0x0800020c |

Workload (oracle-validated, prefill intact):
- T1 compute→SSBO: **GPU WROTE NOTHING** (prefill 0xAA intact)
- T2 transform feedback→TBO: **GPU WROTE NOTHING** (prefill 0x5A intact)
- T3 fragment→FBO: control, still black
- CP control: `A_WROTE=True` — the CP still writes userspace memory.

So these two init-time values are **not the gate**.

### Two facts worth keeping
1. `CP_RB_CNTL` reads 0x0000020c on our device at dump time, while `RINGHW-W` shows it was
   written as 0x20c — consistent, no anomaly.
2. The 0x8e00 block still returns `deafbead` for 0x8e01 / 0x8e0c / 0x8e3b **even with the
   corrected CP_PROTECT table** → the CP_PROTECT fix did not ungate it.  `deafbead` is a
   poison, not a value: that sub-block is not responding.  Working device returns real values.
   This is consistent with the RB/3D sub-block being unpowered (CX domain), not unprotected.
3. **Static diff left: `GPU_CC_CX_GDSCR` = 0xf8222800 (ours) vs 0xf8228000 (working)** —
   bit 23 set only on the working device.  This is the leading candidate for "CP alive,
   RB/UCHE dead".

### Method gap (must close before trusting any further static diff)
The working-device dump was taken with its GPU **busy**; ours with the GPU **idle**.
GDSCR bit 23 and the 0x8e00 poison are exactly the kind of register that may differ purely
because of activity.  The decisive experiment is an **idle** working-device dump:
boot Android, let `gpubusy` settle to 0 (screen off), take `snapshot/dump`, diff again.
If bit 23 is still set at idle → real difference and the gate.  If it is clear → activity
artifact, and the diff must be rebuilt from the idle reference.

Artifacts: `artifacts/flash/287-halium-boot-a6xx-intmask.img`
(sha256 79e75ddf…), `ground-truth/debian-regs-287.txt`.

---

## ADDENDUM 2026-10-24 — the GPU writes memory. The KGSL pivot worked on day one.

Image **288** (`288-halium-boot-kgsl-debian.img`, PLAN-GPU-TRANSITION §B):
the in-tree **KGSL** driver bound on the Debian boot (one config flip +
ifdef guards; KGSL sources byte-identical to pristine ShinyKernel), with
Mesa 25.3 **Turnip** rebuilt for `freedreno-kmds=kgsl` in a bookworm arm64
chroot on mnt-sda.

* KGSL up on Debian: `gpu_model=Adreno640v2`, chip_id `0x06040001`,
  gmem 1 MB, zap loads via PAS on first open — and the **300 s rule /
  a6xx_gpu_load / PM-pinning dance are gone** (those were drm bring-up
  instruments).
* **`tucomp.py` Vulkan compute oracle: 256/256 pattern hits, 4-for-4 runs,
  gpubusy 43→227, zero faults.**  The shader→DDR write that never happened
  in 287 images of drm/msm works immediately under KGSL.
* GL via zink: deployed, Turnip enumerates, but EGL-surfaceless→zink hits
  the DRM-identity match against the msm *display* card (renderD128) —
  one known integration blocker, options listed in the results doc.
* Full details: `IMAGE-288-KGSL-PIVOT-RESULTS.md`.

The drm/msm a640 fork is retired as a component; it remains in the tree,
selectable via `configs/d2q_a6xx_drm_experimental.config.orig-287`.
Track C (Aloha/mainline) remains the long-term path per the transition plan.

## ADDENDUM 2 (2026-10-24 01:30) — OPENGL ES 3.2 WORKS. Headless GPU is DONE.

GL via **zink → Turnip → KGSL**, on the same image 288 boot:

* EGL surfaceless + pbuffer initialises (`EGL 1.5`), with the two-part fix:
  `LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=zink` (EGL's fd-less
  zink fallback path — *not* software rendering) + a 10-line project patch
  to `zink_screen.c` choose_pdev (no-CPU-device fallback to the single
  non-CPU pdev; kept at `.opencode/zink_screen.c.patched-25.3`).
* `kgslgl.py`: **T1 compute→SSBO 64/64 AND T3 fragment→FBO blue-pixel
  readback — both PASS.  Soak: 10/10.**  Renderer string:
  `zink Vulkan 1.3 (Turnip Adreno (TM) 640 (MESA_TURNIP))`, GLES 3.2.
* Vulkan stress: 3 × 4MB dispatch — 1M/1M exact pattern hits each time.
* Zero GPU faults across everything.

Full recipe and details: `IMAGE-288-KGSL-PIVOT-RESULTS.md`.

Remaining (beyond headless GPU): the teardown-time segfault (cosmetic),
on-screen output (display stack — separate track), Track C mainline as
the long-term architecture.
