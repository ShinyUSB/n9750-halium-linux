# beam-PLAN — get off dead center: experiment & strategy design

**Started**: 2026-10-16 (session date per env; corpus labels the same recent work
"2026-11-16").  
**Author**: beam consult subagent.  
**Builds on**: `STATE-END-OF-SESSION-2026-11-16.md`,
`FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md`,
`FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md`,
plus `CONSULT-2-MUSE-SPARK.md`, `CONSULT-3-MUSE-SPARK.md`,
`ANDROID-KGSL-PSP-REFERENCE.md`, `GMU-HFI-BOOT-COMPARE.md`.  
**Baseline**: image 263 (walk tracer, `ibtrace` change-driven, no ibcap hard-reset).
**Standing rules in force**: no hand-built PM4 (guessing encodings already produced
two fabricated claims); no CPU MMIO readback of context-banked a6xx clusters;
treat point-in-time samples and "the device is frozen" claims as unreliable
until `ps` shows D-state; preserve reasoning on disk because a reset kills the
writer.

---

## 0. What is still unknown (one line)

The submission is correct, the SQE walks ring → envelope → leaf and dispatches
`CP_DRAW` (pc=0xd00, pkt=0x70388003), `memptrs->fence` increments (CP→DDR
works), no faults ever — but **no render-backend (RB) write to DDR has ever
been observed in any configuration** (sysmem RB_BLIT clear or GMEM store). The
current evidence cannot separate:

- **H-A (Mesa state never becomes live)** — the descriptors/registers Mesa emits
  are correct on paper but never reach the RB's active context (banking, flush,
  ordering, culling/discard), so the RB has nothing to write; vs.
- **H-B (RB non-functional on this stack)** — the RB is armed correctly but
  cannot perform a DDR store for a deeper reason (addressing mode, clock/power/
  SMMU bank, firmware, kernel config).

Everything below is designed to split H-A / H-B with the smallest possible
cost and risk. Tested theories are cited and TAKEN OFF THE BOARD; do not
re-litigate them.

## 1. Facts established (do not re-derive)

| Layer | Verdict | Evidence file |
|---|---|---|
| Nested IB graph | correct, 3 levels | `FINDING-COMMAND-GRAPH-VERIFIED.md` |
| SQE walks and dispatches CP_DRAW | proven | `FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md` |
| Stream retires, fence lands | proven | same |
| No faults | `int0=0`, `cpint=0` always | same |
| CP→DDR (memptrs fence) | works | `STATE-END-OF-SESSION-2026-11-16.md` |
| RB→DDR | never observed | same |
| Mesa RT descriptor | correct fields | `FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md` |
| RB_MRT colour format / pitch / base / gmem base | correct | same |
| GRAS viewport/scissor 64×64 | correct | same |
| Clear `RB_2D_BLIT_CNTL` (including MASK=0xF) | correct | same |
| UBWC off (linear, 16384 B exact) | ruled out | same (`noubwc` only touches internal BOs) |
| `memptrs_iova`, `RB_RB_RPTR_ADDR`, `RBBM_CLOCK_CNTL` | A640 values | `STATE-END-OF-SESSION-2026-11-16.md` |
| GMEM/sysmem both fail identically | `FD_MESA_DEBUG` matrix | same |
| Culling is live *possibility* (Freedreno docs) | draw with no primitive in tile → silent skip | `FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md` |

## 2. Proven-worthless methods (do NOT repeat)

- **CPU MMIO readback of GRAS/RB/RBBM clusters** — A6xx clusters are
  context-banked; CPU lands on a different context than the one CP wrote.
  `GRAS_2D_DST_TL` read 0 while command stream demonstrably writes it;
  `RBBM_PERFCTR_GPU_BUSY_MASKED` read a constant 0x002f7fff while
  `CP_ALWAYS_ON_COUNTER` advanced. One of the **eleven** misleading
  instruments. (`STATE-END-OF-SESSION-2026-11-16.md` §"Why reading the
  render backend kept failing".)
- **Point-in-time single samples** — sampling rate below the phenomenon
  (submission <~100 µs). Only change-driven 1 µs polling caught the walk.
- **Sampling any register 50 ms late** (the a6xx_submit `msleep(50)`
  then reads) — healthy idle SQE looks identical to a wedged one.
- **`CP_REG_TO_MEM` as currently emitted in this tree** — hard-reset the GPU.
  Root cause is now identifiable (§5, Experiment B): the Adreno PM4 bitfield
  macros in `adreno_pm4.xml.h` predate A6XX (REG shifted 0..15 instead of
  0..17; CNT shifted 19 instead of 18). `CNT(2)` was therefore emitted in
  bits 19..29 ⇒ the hardware read that as `CNT = 4`… far worse, as a
  count well above 2 dwords, which is what (very likely) produced the reset.
  This is **not** "the packet is unsupported on a6xx"; it is a software
  encoding bug in the instrument.
- **`msm_gem_kernel_new()` inside `a6xx_submit()` or inside `hw_init`** —
  deadlocks against `struct_mutex`. Allocated a BO lazily (264) or at init
  (265) — both D-state wedges. Upstream avoids this by calling
  `a6xx_ucode_load()` from `msm_gpu_init()` before `hw_init`; this CAF tree
  calls it from inside `hw_init`. Do not repeat the allocation pattern.
- **`ibtrace` without `udelay` / using `usleep_range`** — sampling slower
  than the phenomenon.
- **Re-setting `ibtrace` to 0 before the probe** or changing ring-buffer size
  without flushing `dmesg`.
- **Declaring the device frozen from a command timeout** without first
  `ps -eo pid,stat,wchan,comm` (process-failure #1 in STATE doc).

## 3. The recommended (and ranked) plan in one paragraph

Do **not** build the replay oracle yet, and do **not** edit the kernel again
until Experiment B's single-bitfield correction has been tried. First run
Experiment A purely in userspace (perf counters / `FD_MESA_DEBUG` matrix /
perf-monitor GL): it costs nothing, risks the device not at all, and can
already tell us whether the RB ever saw work. If A is ambiguous on the armed
question, apply Experiment B's one-line-class encoding fix to the existing
`ibcap` path: it re-uses `memptrs`' tail buffer (no new allocation → no new
deadlock class) and uses the CP's own view, which is the only cluster readout
that is not context-banked. Only if B shows the RB is armed *and* never
writes do we build Experiment D (replay of Mesa's own bytes). Experiment C
variants (debugfs, tracepoints, other device, Halium comparison) are parallel,
cheap, and should run throughout because they cost reboot-at-most. Section 10
gives the numbers; Section 12 the decision tree.

## 4. Standing environment facts for userspace work

- Mesa root: `/tmp/opencode/mesafull` (Mesa 22.3.6, freedreno Gallium).
- `FD_MESA_DEBUG` flags include `gmem/sysmem,flush,dclear,ddraw,direct,
  inorder,noblit,noubwc,notile,layout,perf,deqp,nosbin,lrz/nolrz`. `TU_DEBUG`
  does **nothing** on the Gallium path (consult 2 retracted an entire round
  of conclusions for confusing them).
- Direct rendering target is `/dev/dri/renderD128`; GBM platform works,
  surfaceless/device EGL segfault for this build. Harnesses in
  `/home/usb/artifacts/gpu-bringup/harness/` (`egl-gbm-fbo.py`, etc.).
- Open: `MSM_INFO_GET_IOVA` (msm_drv.c:1715) returns `args->value`.
- Kernel: `ShinyKernel-gpu-exp` (current baseline); `a6xx-build` (second
  tree). Custom CAF tree, Adreno 640 / a6xx.
- Boot images are numbered (263 = current good tracer). Flashing costs a
  reboot and, if the kernel deadlocks, a much longer recovery. Price every
  kernel edit at "one device-week of risk" until proven otherwise.
- Debugfs nodes available when root: `/sys/kernel/debug/dri/0/gem`,
  `gem_names`, `/sys/kernel/debug/clk/clk_summary`,
  `/sys/kernel/debug/dma_buf/bufinfo`, `/sys/class/devcoredump/`.
- ftrace only `nop` tracer; no CPU ETM (`CONFIG_CORESIGHT_SOURCE_ETM4X` not
  set) and no coresight for GMU; `perf_event` only if kernel built with it.
- `/sys/kernel/debug/dri/0/rd` exists; `msm.rd_full=1` module/boot param
  would capture more (buf snapshot). Debugfs root denied without root.
- Config knob worth trying: `CONFIG_DRM_MSM_REGISTER_LOGGING` for register
  read/write logging (rebuild).

## 5. Experiment A — userspace-only: is the RB ever armed? (cost 0)

**Question it answers**: Does any RB-side engine report work for a submitted
draw/clear, and do `FD_MESA_DEBUG` modifiers change the outcome? If the RB
reports busyness, H-A becomes much less likely; if nothing ever reports work
while stays all-zero, we get the first hardware-side confirmation that the
RB ignored the stream.

**Sub-experiments** (all run against baseline 263, no flash):

### A.1 Performance counters through GL
Mesa exposes `GL_AMD_performance_monitor` when `FD_MESA_DEBUG=perfcntrs`.
1. `egl-gbm-fbo.py` extended to create a context with a perf-monitor query
   around `glClear` + `glDrawArrays` (clear, then one full-screen triangle).
2. Read counters before/after with multi-draw. Look for:
   - any "RB busy / pixels / memory store / CCU write" counter > 0 for a run
     that still reads back all zeros;
   - whether counters follow submit count (1, 11, 60 draws) — proves they are
     live rather than stale.
3. If the device's perf-monitor exposes only registers that require `CP
   _REG_TO_MEM`, the query itself exercises that path safely (Mesa's
   encoding is correct, per its XML). That's the point: let Mesa do the
   encoding we are not allowed to guess.

**Deliverable**: one table: draws N → counter deltas. Capture raw output under
`consult/a1-perfcounters.txt`.

### A.2 Command-stream capture audit (no run-time register risk)
1. As root on device: `cat /sys/kernel/debug/dri/0/rd > /tmp/capture.rd`
   (size first). If the file is empty, try `rd_full=1` only at the next
   planned reboot (policy knob, not a kernel edit).
2. Decode offline with Mesa `cffdump` built from `/tmp/opencode/mesafull`
   (`src/freedreno/decode/cffdump.c`). The current decode docs already cover
   the nesting; just confirm (manually or via cffdump) that the state prologue
   and RT descriptor survive into the *first* level dump (cffdump follows IBs).
   This does not prove RB writes, but it does catch "Mesa declared a surface
   but never referenced its IOVA".
3. Cross-check with `/sys/kernel/debug/dri/0/gem` + `gem_names` to confirm the
   RT BO's IOVA matches `0x7_00025000` (the one in the descriptor).

### A.3 `FD_MESA_DEBUG` modifier matrix (reprun)
Re-run the clear-only and draw harnesses with single flags, one at a time, and
record **only** whether the RT is non-zero (not performance):
```
sysmem, gmem, flush, dclear, ddraw, direct, inorder, noblit, noubwc,
notile, nolrz, serialc, deqp, hiprio
```
Plus pairs `{flush,sysmem}` and `{flush,gmem}`. The corpus already ran `FD
_MESA_DEBUG` variants but mostly as a full matrix without isolating; the
standing lesson is that `gmem`/`sysmem` behave identically. **Stop as soon as
one variant produces a non-zero RT** — that is the H-A fingerprint and more
informative than the run list.

### A.4 Cheap injection of state pressure inside GL
- Draw one big triangle (not a 1-vertex-ish quad) with depth writing enabled,
  alpha blending disabled, viewport exactly matching the surface; no custom
  shader (default “no shader” path if available) and a minimal vertex shader.
  If even a no-shader/full-screen draw reads zero, culling-by-tile becomes
  unlikely (the primitive covers every tile).
- Bind an actual texture and sample it. If `MAX_TEXTURE_IMAGE_UNITS=0` is real
  (`CONSULT-3`), a fragment shader sampling a texture fails to compile — a
  loud signal that the screen object is partially initialized, which is
  H-A-style and independent of the RB.

**Failure mode / where A does NOT discriminate**: A can show the RB is busy
but cannot show *whether the RB context was armed with the correct RT
address*; culling and false-busy counters remain live. A is triage, not proof.

## 6. Experiment B — repair `CP_REG_TO_MEM` encoding and finish `ibcap`

**Highest-value single change left on the table.** It is genuinely a fix of the
instrument, not a new instrument: the macro mismatch is verifiable against
Mesa's own XML (`REG` 18 bits, `CNT` 12 bits starting at 18, `64B` at 30).
Our tree's header has `REG` 16 bits (0x0000ffff) and `CNT` shifted 19
(0x3ff80000). For a 2-dword read that is `CNT(2)<<19` (bits 21/22) instead of
`1<<18` (magnitude 4). Applied to registers in the `0x8xxx` range this (most
plausibly) is the hard-reset mechanism in 266, not `CP_REG_TO_MEM` itself.

**Protocol** (one kernel patch, no allocation):
1. In `ShinyKernel-gpu-exp`, audit **all** use sites of `CP_REG_TO_MEM_0_REG/
   CNT` (`grep -rn`), not just `a6xx_gpu.c`. Fix the macros to match Mesa's
   field layout **or** bypass macros in `a6xx_ibcap_emit` and open-code the
   dword from `adreno_pm4.xml` as Mesa does. Prefer the second: it removes the
   macro rather than "fixing" a header every caller depends on. Add a build-time
   `#error` if `CP_REG_TO_MEM_0_CNT(1)` doesn't round-trip to `1<<18`.
2. Before running on device: run the fixed emit **only for ONE register**
   (`REG_A6XX_RBBM_STATUS`, the least dangerous) for at least one boot, with
   `ibcap=1`, and confirm the capture prints and the phone survives. Only then
   enable the full `ibcap_regs[]` set.
3. Pin `ibcap=1, ibtrace=1`, submit 1, 11, 60 draws, read dmesg. Verify:
   - `a6xx_ibcap_report` prints 2 values per register, not zero/garbage.
   - `gpubusy_delta` behaviour unchanged (i.e., we did not perturb the path).
4. The registers to capture (already in `ibcap_regs[]`) become a CP-view
   version of Experiment A's questions:
   - `GRAS_2D_DST_TL/BR` (and for the draw path `GRAS_SC_VIEWPORT_SCISSOR
     _TL/BR` — add them to the table):
     - if **zero in CP view** while Mesa demonstrably emits them → H-A: state
       mutation is not latching into the active RB context (culling/flush/
       banking);
     - if **non-zero but no DDR write** → H-B: RB armed, RB failed to store.
   - `PERFCTR_GPU_BUSY` delta vs baseline (does the RB ever claim busy).
   - `RB_ADDR_MODE_CNTL` (64-bit mode) and `RB_CCU_CNTL`.
   - `SP_FS_CTRL_REG0` (shader armed?).
   - New additions to the table worth ~2 lines: `REG_A6XX_CP_INTERRUPT_STATUS`,
     and `REG_A6XX_RBBM_INT_0_STATUS`. These three let us correlate "CP drew
     vs RB cleared" on one packet rather than relying on one CPU late sample.

**Cost**: one kernel rebuild + one flash; risk of another hard reset is real but
bounded by A.1's single-register gate, which uses the same `CP_REG_TO_MEM`
encoding Mesa already runs every day for queries. **Failure definition**:
- If the corrected encoding still hard-resets → a6xx `CP_REG_TO_MEM` in this
  tree's WFI ordering needs a `CP_WAIT_FOR_ME`/`CP_WAIT_FOR_IDLE` before it;
  do **not** guess compat bits. Enter B-fail branch (store-only observation via
  devcoredump, §7.B).

**Why B is better than "instrument pc=0xd00" directly**: we are *already* running
*inside* the window — `ibtrace` polls at 1 µs, and `ibcap` packets are appended
*after* the leaf IB runs, so they capture post-draw state in the CP's own
context. The one thing wrong is the register-packet encoding, and we now know
what it is. No new allocation class, no struct_mutex, no device-of-the-hour.

## 7. How to read the render backend *without* CPU MMIO readback

Three sub-methods; use in this order:

### 7.A Mesa drives `CP_REG_TO_MEM` safely (already available)
Perf counters (`FD_MESA_DEBUG=perfcntrs`) cause Mesa to emit CP_REG_TO_MEM
with a correct kernel/macro-free encoding on the same packet stream. Reading
the query memory afterward is userspace-only. Use for (A.1). **This is the
only in-band capture path that never required a kernel rebuild.**

### 7.B Controlled state capture via the a6xx crashdumper
Upstream `a6xx_gpu_state_get()` drives the **CP crashdumper**: a CP script
that reads registers/clusters *in the CP's own context* and writes them to a BO
— exactly the tool the kernel uses to avoid the context-banking problem. The
components (`msm_gem_kernel_new` inside `a6xx_gpu_state_get` is allocation;
but that allocation happens at *capture time*, not inside `a6xx_submit`, so it
does not take `struct_mutex` the same way — verify with `ps` on first trial,
do not assume):
- Entry point already in tree: `.show_state = a6xx_show` (basic counters only).
- Full state arrays are built by `a6xx_gpu_state_get()` in the crashdumper.
- Invoking it without a fault is not supported out of the box; options:
  1. Force a controlled hang safely after the draw: submit a ring with a
     self-waiting packet (`CP_WAIT_REG_MEM` comparing a location that will
     never equal) so the CP stalls at a known point; the kernel's hangcheck
     fires (`a6xx_recover`), which calls `a6xx_gpu_state_get()`, writing the
     devcoredump. Risk: high but mechanical; recoveries have worked all
     session. **Never** do this on a session that hasn't been exercised at
     least once with `a6xx_show` succeeding.
  2. Or add a sysfs "snapnow" attribute (≈30 lines) that calls
     `a6xx_gpu_state_get()` without a fault and prints the indexes. This is
     a kernel edit but a much safer class than (B): no registration-field
     encoding, no new alloc-in-hot-path; alloc happens in debug path.
- Command to consume: `cat /sys/class/devcoredump/devcd/data > /tmp/dump` then
  `crashdec >/tmp/decoded`. Cluster readout (including GRAS/RB) is where the
  banked truth lives.

**Highest decode value** and standard freedreno practice; the project's reason for
not using it has only been "it can't run on demand", which (2) above addresses.

### 7.C Coredump-from-hang (no code) as a last resort
A real fault + `a6xx_recover` → standard `/sys/class/devcoredump/`. We have
managed our entire bring-up *without* a fault, which is itself evidence; but if
an instrument (B) ever introduces one, do not throw the dump away — decode it
with `crashdec` before reflashing. This is free evidence.

## 8. Experiment D — replay of Mesa's own bytes (the "oracle")

**Honest pre-evaluation before building anything.**

### What it is
Capture every BO in one representative Mesa submit (envelope + leaf + RT +
shaders + state), copy each to a new BO we own, get IOVAs with
`MSM_INFO_GET_IOVA`, patch (RT IOVA, nested IB pointers + sizes,
relocations), re-submit via the normal DRM submitqueue. No hand-built PM4
encodings: the bytes are Mesa's own, and `MSM_INFO_GET_IOVA` is the UAPI hook
for mapping handle→IOVA. This is the only experiment in the list that can, in
principle, answer "is the problem *Mesa's bytes in situ* or *the RB*".

### Why a *positive* replay is decisive and a *negative* is not
- **Positive** (replayed clear writes the RT): Mesa's state is the defect —
  either ordering, context lifetime, bo naming/reloc happening *during* normal
  submission but elided in the replay. H-A likely. Actionable: diff the two.
- **Negative** (replayed clear also all-zero): ambiguous. Dangling references
  (memptrs/scratch/rptr addr, shader bo reloc, GMU-related deferred state)
  can make a *faithful* replay impossible without re-executing the whole
  per-submit CPU-side state, at which point you are rebuilding Mesa.
  Negative does NOT prove H-B.

### Hidden cost drivers (cite, don't ignore)
- Submit queue/A space: handle→iova mapping and relocations; `msm_gem_submit`
  validates cmd sizes in dwords (do not repeat the bytes-vs-dwords error #10
  from `FINDING-COMMAND-GRAPH-VERIFIED.md`).
- The capture: need the ring + every IB; without `msm.rd_full=1` or a Mesa
  tap, we cannot reliably list every BO. Can obtain partial via debugfs
  `gem`/`gem_names`, but patching without a complete relocation table is a
  guaranteed miss.
- Nested pointers in IBs are absolute GPU VA: patching must be recursive, and
  enumerate all relocs (the original showed 13/13 resolve, so the count is
  known and small, but we must not miss one).
- Lifetime: `memptrs` and `scratch` relocation would also need patching if we
  make the replay self-contained, or re-pointing (cheaper: keep original's
  memptrs IOVA? risky, pollutes original state).
- Device risk: a malformed replays can take down the GPU exactly like 266.
- Engineering: several hundred lines of Python; the project's own harness
  history shows that relocation bookkeeping is where mistakes live.

### Verdict on (D)
**Defer. Do not build until after Experiments A and B.** Rebuild D only if B
shows the RB is *armed with correct state* and still produces zero bytes, and
even then build the **narrowest possible version first**: a *clear-only* replay
(solid-colour `RB_BLIT` path, ~237 dwords envelope, no shaders, no vertex
state). The clear path is the cleanest H-A/H-B discriminator because it does
not depend on draw culling or tile geometry: if even a correctly-encoded solid
clear cannot reach DDR, H-B is strongly indicated and no shader debugging will
help. If the clear replay writes, Mesa's clear emit is the bug. Draws (and
culling) are a separate, later oracle. Do **not** ship a "full draw replay"
as the first version.

### Negative-result handling
If even the minimal clear replay is negative, record it as "RB does not write
from a replayed stream either" and **stop**. Do not expand D into more draws
or more clever patching without new evidence — that is the trajectory that led
to three device-breaking kernel changes gaining nothing (standing process
failure #2 / instrument inflation).

## 9. Cheap, non-kernel-breaking alternatives (parallel, always on)

Ranked cheap-first. None modify the kernel; most only need on-device root or a
userspace binary.

### 9.1 Debugfs and procfs (no reboot)
- `/sys/kernel/debug/dri/0/gem` + `gem_names`: enumerate every BO's size/name
  and mmap offset. Use to verify names ("fs:...", "cmdstream", "gmem") and
  RT size. Costs minutes. This is the foundation for Experiment D if we ever
  build it.
- `/sys/kernel/debug/clk/clk_summary`: confirm GX domain/clock on across the run
  (`CLOCK-GX-DOMAIN-OFF.md`, `GMU-*` history — collapse into one check: is
  the GPU actually clocked in the submit window?).
- `/sys/kernel/debug/dma_buf/bufinfo`: BO pin/attachment sanity.
- `/sys/kernel/debug/tracing` available_tracers: only `nop` (known). Setting a
  tracer requires root and is valueless here. Do **not** spend effort on
  ftrace unless `CONFIG_FTRACE` got enabled since `IMAGE-83-84` (it hadn't
  then). One command to check; skip if only `nop`.
- `/proc/interrupts`: GMU/HFI vs core IRQ (332) — confirm the core IRQ still
  never fires. If it *does* fire on baseline 263 in this regimen (new since
  image 184 era), the entire picture changes; re-read `MESA-RENDER-ATTEMPT-2`.

### 9.2 Perf and PMU
- `perf stat -e` / `/sys/bus/event_source` depends on kernel PMU support;
  check once. If present, PMU hardware counters can corroborate `PERFCTR_GPU
  _BUSY`, which only CP-side CSQ readouts can be trusted anyway.
- CPU-side cost not relevant: renders are all CPU time waiting.

### 9.3 Different Mesa builds (low risk, high potential signal)
- Try the other Mesa trees already on disk (`mesaMain`, `mesa223`/`mesa2236`,
  `mesa-src`) — same source family. A newer mainline freedreno may emit
  `CP_WAIT_FOR_IDLE` or different ordering; if a newer Mesa makes the RT
  non-zero on the *same* kernel/image (263), H-A is proven and we know what
  changed (diff command streams). This is one of the highest-value *cheap*
  moves and is not in the corpus's history.
- env toggles without rebuild: `FD_MESA_DEBUG` (A.3), `MESA_NO_ERROR=1`,
  cache disable flags (already tested zeros-cache-empty; do not repeat unless
  a *new* state changes).
- Build/install isn't needed if we swap `LD_LIBRARY_PATH` between the
  pre-built out trees — check which have `libGLESv2.so` built.

### 9.4 Other hardware as control
- Note from `ANDROID-KGSL-PSP-REFERENCE.md`: KGSL on the *same* device boots
  the GMU/PSP firmware fine. A KGSL-side render (if any Android userspace
  stack usable) can tell us whether the RB can write DDR *at all* on this
  silicon. Halium rootfs (`/home/usb/n9750-halium`, `/home/usb/halium-install`)
  plus production ShinyKernel (`KGSL=y`) was the prior vehicle. If a
  minimal GLES app can be run under that stack (software renderer aside), one
  captured GMU log comparing against our DRM path is very informative. Cost:
  reboot, Halium mount complexity, kernel swap. **First ask**: does the KGSL
  test harness already exercise GL? Probably not.
- A *different* Adreno 6xx device (even a cheaper one on the desk, if any)
  running mainline upstream would be the strongest H-B control and the only
  way to kill "our CAF tree's a6xx config is wrong". We do not currently have
  one in evidence — check lab inventory before assuming.
- Devcooling: if another Note10+ / S10+ (SM8150) is available, flash the
  *same* image 263 and confirm the symptom reproduces (a null result on new
  hardware is still evidence; a *different* result is decisive).

### 9.5 Known-good stack comparison on the same kernel (Halium)
`ANDROID-KGSL-PSP-REFERENCE.md` is the in-house control: production kernel +
KGSL + Halium + Debian runs GMU to BABEFACE. The CSP differences (WFI_CONFIG
etc.) are *documented*. The render path is not currently the object there.
Useful framing:
- GMU is alive; H-B is *not* "GPU can't be brought up".
- If the same production kernel with KGSL always renders in Android but our
  DRM tree does not, the deficit is in our kernel↔Mesa contract, not silicon.
  Compare UAPI submission flags, SMMU context bank, memory attributes.
- Cost: high but zero risk to baseline if done on a separate boot image.

### 9.6 Clock/power sanity
- `clocks-GX-DOMAIN-OFF` history and `A640-*` — ensure GX is on in the
  submit window. Check `clk_summary` during a run; if GX collapses to 0
  between init and submit, the RB is unpowered and H-B-type; re-evaluate
  `a6xx_gmu_set_active`/power domain. This is **not** the headline theory
  (GMU boots, CP writes work) but is a $0 check.

## 10. Ranking by (decisiveness × cheapness) / risk

| # | Experiment | Cost | Device risk | Discriminates H-A vs H-B? | Rank |
|---|---|---|---|---|---|
| A.1+A.3 | Perf counters + FD_MESA_DEBUG matrix (userspace) | 0 (hours) | ~0 | Medium (RB busy signal) | 1 |
| A.2 | `cffdump`/`gem` IOVA cross-check | 0 | 0 | Low-medium (state in situ) | 2 |
| B | Correct `CP_REG_TO_MEM` bitfields + `ibcap=1` | 1 rebuild/flash | Medium (bounded by single-reg gate) | **High** (armed-vs-wrote) | 3 (was #1 value; ranked here for risk, run ASAP after A gate) |
| 9.3 | Swap newer Mesa out-tree | 0–low | ~0 | Medium (if works → H-A) | 4 |
| 9.4/9.5 | KGSL/other device/Halium control | Medium-High (reboot) | Low | Medium (controls RB) | 5 |
| 7.B | sysfs `snapnow` via crashdumper | 1 rebuild/flash | Medium | High (banked registers) | 6 |
| D (clear-only) | Replay of Mesa bytes, clear path only | High (weeks) | Medium-High | **High but only positive branch decisive** | 7 |
| 7.C | Controlled hang → devcoredump | 0 code | High | High | 8 (only if already instrumenting) |
| Full draw replay D | Full replay oracle | Very high | High | Negative ambiguous → **poor value** | 9 |
| Anything changing SMMU/clk/GPU_SET etc. without a hypothesis | High | High | None | Do not |

Rule of thumb to apply through the ranking: prefer 3 experiments that decide
something over 20 that confirm something. (A.1/A.3/B/9.3 are the four worth
running before D.) If, at any point, two independent instruments agree the RB
is armed and still writes nothing, **stop tuning Mesa state** and treat the
stack as RB-nonfunctional for H-A/H-B purposes — write down the result.

## 11. What I would NOT do

- **More CP outer loop/counter reading via CPU MMIO** (context-banked trap).
  Already once (image 263/264). Produces confident wrong answers (fact #1 in
  the eleven). Stop.
- **Hand-built PM4 by guessing encodings.** Forbidden rule; two fabricated
  claims already; now only permitted *copying* real captured bytes and
  patching their pointer/address fields, never inventing a packet.
- **Any kernel allocation inside `a6xx_submit` or `hw_init`** (deadlock class
  264–265). Allocation for instrumentation belongs in the ordinary DRM init
  path or in the debug path (7.B), not the submission hot path.
- **`CP_REG_TO_MEM` with the current `0x3ff80000` (shift-19) macro** in any
  form until corrected or open-coded from `adreno_pm4.xml` — it hard-reset
  the GPU in 266 with a probable count overrun, not a protocol failure.
- **Build (D) as a draw-level replay first.** Clear path has to write first
  or nothing is worth patching; a full-draw replay is a month of
  relocation bookkeeping to learn something an A/B/better instrument can
  learn in days.
- **Expanding the corpus / instruments count.** Eleven already produced
  confident wrong answers by measuring the wrong layer. New instruments need
  to live in (a) `ibtrace`-triggered, (b) CP-driven readout (CP's own context),
  (c) post-fence, else skip.
- **Blaming `MAX_FREQ`, banned state, or ordering of `open`/`GET_PARAM`.**
  Muse Spark killed those (`CONSULT-2` / `CONSULT-3`). Dead. Do not relabel
  them as "maybe separate".
- **"The phone is frozen" from a timeout** without `ps` (process failure #1).
  The +5s `show_state` and SSH often survive when a kernel thread is D-state.
- **Chasing culling with more shader work** before the clear proves armed.
  Clear has no culling story (solid RB_BLIT); if clear dies, draw culling
  cannot be the common cause.
- **Assuming the same physical device is available** without verifying.

## 12. Decision tree

```
START: baseline 263, ibtrace=1. Run (A) first.

A.1 perf counters (after submit) / gem name audit (A.2)?
  ├─ any RB-relevant counter > 0 on a readback-all-zero run?
  │   ├─ YES → RB ran pixels but did not store → H-B-ish.
  │   │        Run B to confirm RB's armed state (gras_tl/rb_addr) via CP
  │   │        view. If armed + zero store → escalate H-B (clk/address/
  │   │        CCU). Do NOT keep tuning Mesa.
  │   └─ NO  → RB never got busy; continue
  └─ reads intermittent with N draws?
      └─ likely a broken counter or wrong register; treat as inconclusive

FD_MESA_DEBUG single-flag run produces non-zero RT?
  ├─ YES → H-A fingerprint (state becomes live under specific modifiers).
  │        Diff the successful vs. failing streams (cffdump) and isolate
  │        ( ordering/flush/bind ).
  └─ NO  (all identical zero) → GMEM/sysmem identical → local state defect
       less likely; proceed to B.

A.4 full-screen triangle + no-shader + texture?
  ├─ texture shader fails to compile → partial screen init (H-A-ish),
  │   independent bug; record and follow up separately.
  └─ draw still zero, busy still 0 → not a tile-cull; proceed.

Experiment B (corrected CP_REG_TO_MEM encoding)?
  ├─ hard-reset even with one register?
  │   └─ YES → encoding mismatch not the cause; try 7.B `snapnow` +
  │             CP_WAIT_FOR_ME ordering patch (bounded) OR 7.C hang dump.
  │             If both fail, accept that RB view is unreachable live on this
  │             stack and document RB-write as unresolved.
  ├─ prints capture, CP view shows GRAS_2D TL/BR and RB_ADDR_MODE non-zero
  │   and SCISSOR correct, but DDR still zero
  │   └─→ H-B strongly indicated (armed, did not store). Run 9.6 clocks + 9.5.
  │       Consider clear-only replay (D-min) for a "does nothing else help"
  │       confirmation; otherwise stop and write the conclusion.
  └─ CP view shows zero TL/BR (or RB_ADDR_MODE 0) in at least one of the
     contexts while CPU stream contains the write
     └─→ H-A: state not latching. Investigate flush/order/context banking
         via Mesa mods (`flush`, `dclear`, `ddraw`, `direct`, `inorder`,
         explicit waits). Build D-min only to prove the inert bytes need
         a change.

Experiment 9.3 (newer Mesa) changes the result?
  ├─ YES → diff Mesa versions' fullpaths; H-A solved by what changed.
  └─ NO  (still all-zero) → strongly reinforces H-B; combine with B result.

If all of A, B, 9.3 say "armed but zero": write the supported conclusion:
  "submission correct, SQE executes draws, CP writes DDR, RB produces no write
  in any tested config (including replayed clear) — cause unresolved; not
  fixable from this hardware/software stack without silicon-level work."
  Do NOT invent another partial mechanism.

If B's replay variants or D-min produce a POSITIVE (replayed clear writes):
  → Mesas's state is non-live in situ (ordering/lifetime, not encodings);
    find the missing synchronization; re-run A under the fixed path.
```

## 13. Epistemic boundary

This stack may simply not answer "why does the RB not write" from here. The RB
is a silicon micro-engine whose context-banked registers we cannot read from the
CPU; our one chance to read them requires either the fixed `CP_REG_TO_MEM`
instrument, Mesa-driven queries, or an on-demand crashdumper. None of them
have run yet on a non-faulting path with correct encodings. If all three fail
to execute (encoding error, deadlock, or register protocol), then the project
has reached the honest limit of what this hardware/software combination can
discriminate, and the right output is a one-paragraph statement, not another
plan. That is an acceptable stopping point and better than manufacturing a
fourteenth "root cause".

## 14. Immediate next 72 h (checklist)

- [ ] Check root availability on device; `id`, then `ls /sys/kernel/debug/dri/0`.
- [ ] A.1 implement perf-counter reads into `egl-gbm-fbo.py` copy; run
      N=1/11/60; save raw under `/home/usb/artifacts/gpu-bringup/consult/a1`.
- [ ] A.2 `cat /sys/kernel/debug/dri/0/gem*` to verify RT IOVA == 0x7_00025000.
- [ ] A.3 one-by-one `FD_MESA_DEBUG` matrix, stop on first non-zero.
- [ ] A.4 full-screen triangle + shader/texture probe.
- [ ] Check `/home/usb/artifacts/gpu-bringup/harness/` for any already-ready
      version of the above; reuse, don't reinvent.
- [ ] Look in `/tmp/opencode/mesafull` / `mesaMain` for a prebuilt
      `libGLESv2.so` newer than 22.3.6 usable for 9.3 (no rebuild first).
- [ ] If A is all-zero + no armed signal → queue B: fix macros/open-code,
      single-register gate, then full capture.
- [ ] Only after B returns armed-but-zero → decide on D-min vs. stop on H-B.
- [ ] Append every run's dmesg capture to `consult/` (reset kills writers).

## Appendix: cross-reference of files read for this plan

- State: `STATE-END-OF-SESSION-2026-11-16.md`
- SQE finding: `FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md`
- Command graph: `FINDING-COMMAND-GRAPH-VERIFIED.md`
- Mesa decode: `FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md`
- Process corrections: `CONSULT-2-MUSE-SPARK.md`, `CONSULT-3-MUSE-SPARK.md`
- Control/other stacks: `ANDROID-KGSL-PSP-REFERENCE.md`,
  `GMU-HFI-BOOT-COMPARE.md`
- Register mapping/PM4: `/tmp/opencode/mesafull/src/freedreno/registers/adreno/adreno_pm4.xml`
- In-tree code (verify during B): `ShinyKernel-gpu-exp/Kernel/drivers/gpu/drm/msm/adreno/a6xx_gpu.c`
  (`a6xx_trace_burst`, `a6xx_ibcap_emit`, `a6xx_ibcap_report`, `a6xx_submit`);
  `adreno_pm4.xml.h` (macros).
- Kernel facility reference: upstream `drivers/gpu/drm/msm/adreno/a6xx_gpu_state.c`
  (crashdumper), Mesa `src/freedreno/decode/crashdec.c`,
  `docs.mesa3d.org/drivers/freedreno.html` (context banking).
- Never-again rules cite: `STATE-END-OF-SESSION-2026-11-16.md`
  "Eleven instruments … produced confident, wrong answers".
