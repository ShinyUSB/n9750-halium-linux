# FINDING: the shader ISA buffer is never submitted — the draw points at unmapped memory

Date: 2026-10-18
Image: 270 (base 263 + page-table prologue). No new kernel code was needed for this
measurement — `ibdump` is a **runtime** module parameter.

## Verdict

The submission carries a **64-bit GPU address that no submitted BO provides**, and
that address sits in the a6xx *shader descriptor* at the same dword offset in both
the render and the compute path. The shader program buffer is present in the Mesa
process but **absent from the GEM submission**, so the GPU is pointed at memory
the submission never maps.

This is the first mechanism in the project that explains, with one cause:
RB writes dead, SP (compute) writes dead, no faults, CP writes to DDR fine, and
Mesa's state live in the registers.

**Status: the measurement is proven; the identification of the dangling address as
the shader ISA is a strong inference that still needs one confirming step**
(see "What is not yet proven").

## The measurements

All taken on `FD640` (real GPU), with `ibdump=1` set at runtime via
`/sys/module/msm_drm/parameters/ibdump`.

### 1. The shader ISA is present in the Mesa process

`vmascan8.py` (new harness, this session) walks `/proc/self/maps` inside the Mesa
process after a draw and reports every shared `/dev/dri` mapping with its DRM
file offset and first 32 bytes:

```
0x007f7fae2000 4096  0x10217b000  8  000001200100904300000120000010470c0b0a3f04c04c20...  <<< VS MAGIC
0x007f7fae3000 4096  0x10217a000  8  000001200100904300000120000010470c0b0a3f04c04c20...  <<< VS MAGIC
0x007f7fae4000 4096  0x102179000  8  5634123f00c04c200000803e01c04c200000003f02c04c200000803f03c04c20  <<< FS MAGIC
```

The magic constants (`0x3F123456` in the FS, `0x3F0A0B0C` in the VS) are baked
into the shaders and appear verbatim in the compiled a6xx ISA, so these mappings
are unambiguously the shader programs, and they are populated.

### 2. Those same buffers are not in the submission

The kernel's submit-time dump for the same submit (seq=21):

```
bo[0] iova=0x700023000 mmoff=0x102177000 sz=4096
bo[1] iova=0x700026000 mmoff=0x102187000 sz=20480
bo[2] iova=0x700039000 mmoff=0x10218c000 sz=4096
bo[3] iova=0x70002f000 mmoff=0x10217d000 sz=4096
bo[4] iova=0x700018000 mmoff=0x10216c000 sz=4096
bo[5] iova=0x700030000 mmoff=0x10217e000 sz=4096
```

`mmoff` 0x102179000 / 0x10217a000 / 0x10217b000 — the three shader-ISA mappings
from (1) — **do not appear**. The kernel received 6 BOs; the ISA is not one of them.

### 3. The command stream points at an address with no BO behind it

The tree's own image-260 DANGLING IOVA scan fired:

```
BOS[seq=21] DANGLING: bo[3] dword 36 -> iova 0x700027000 (no submitted BO provides it)
BOS[seq=21] DANGLING: bo[3] dword 74 -> iova 0x700000007 (no submitted BO provides it)
```

The scan reads 64-bit values as dword pairs (`sc[j]` = LO, `sc[j+1]` = HI) and only
considers values in the aperture range `0x700000000..0x800000000`. For bo[3]:

```
[33] 48b4c08a
[34] 00501330
[35] 00200040
[36] 00027000   LO  \
[37] 00000007   HI  /  0x00000007_00027000 = 0x700027000   <-- dangling
[38] 00000800
[39..43] 0
[44] 48b4ca86
[45] 00026000   LO  \
[46] 00000007   HI  /  0x00000007_00026000 = 0x700026000   <-- = bo[1], submitted
[47] 00004001
```

bo[3] is the a6xx **shader descriptor**. It carries exactly one GPU address in
the aperture range, and that address has no submitted BO behind it. The second
address in the same descriptor (0x700026000) *is* submitted (bo[1], the fixed
20480-byte buffer) — so the descriptor's other reference is fine; only one is
dangling.

### 4. The compute path shows the identical signature

Same run of `wrt2.py` (T1 compute + T2 transform feedback + T3 render):

```
BOS[seq=4] DANGLING: bo[3] dword 36 -> iova 0x7000d0000 (no submitted BO provides it)
BOS[seq=5] DANGLING: bo[8] dword 2/30/36/42/48/60/66/80/86/88/90/92 -> ...
```

**dword 36 in bo[3] again** — the same structural position as the render path.
One mechanism covers both the RB and the SP write path, which is exactly the
property the project needs (the defect is shared between them).

## Why this explains everything else

| Observation | Explained how |
|---|---|
| RB → DDR writes never observed | the render backend has no program to execute |
| SP (compute) → DDR writes never observed | same, for the compute shader |
| No SMMU / GPU fault ever raised | the fetch is dropped silently, not reported |
| The CP still writes DDR (`memptrs->fence` increments) | the CP's own stores use a submitted BO |
| Mesa's state is live and workload-tracking in the aperture registers (image 267) | the CP applies state; the shader never runs |
| The SQE executes `CP_DRAW` and `CP_LOAD_STATE6_FRAG` (walk tracer) | the command stream is consumed; the program it points at is absent |
| Command stream verified correct field-by-field against real XMLs | the stream is fine; the *address* in it is not backed |
| A page table is installed and correct (image 270) | translation is not the problem; the mapping is *absent* |

## Mechanism in Mesa (source analysis)

`src/freedreno/drm/freedreno_ringbuffer.h:309` — `OUT_RELOC()` has two
mutually exclusive paths:

```c
#if FD_BO_NO_HARDPIN
   uint64_t *cur = (uint64_t *)ring->cur;
   *cur = iova;
   ring->cur += 2;
   fd_ringbuffer_attach_bo(ring, bo);          /* attach, NOT a reloc */
#else
   struct fd_reloc reloc = { .bo = bo, .iova = iova, ... };
   fd_ringbuffer_reloc(ring, &reloc);          /* record a reloc */
#endif
```

`FD_BO_NO_HARDPIN 1` is defined for the whole a6xx driver
(`fd6_draw.c:28`, `fd6_emit.c:28`, `fd6_compute.c:27`, `fd6_const.c:25`,
`fd6_context.c:28`, `fd6_gmem.c:28`, `fd6_blitter.c:28`, `fd6_blend.c:28`), so
a6xx always takes the **hardpin** branch.

Consequences:
- The IOVA is written **directly into the command stream** — which is why the
  dangling address is visible in the stream at all. ✓ matches the measurement.
- The BO is *attached* to the ringbuffer rather than recorded as a reloc.
- `fd_ringbuffer_sp_emit_bo_nonobj()` (submit ring) → `fd_submit_append_bo()` →
  the BO reaches `fd_submit->bos[]`. ✓
- `fd_ringbuffer_sp_emit_bo_obj()` (ringbuffer **object**) → the BO is appended to
  the object's own `reloc_bos` list.
- The a6xx shader program is emitted with
  `OUT_RELOC(ring, so->bo, 0, 0, 0);` twice
  (`fd6_program.c:132` and `:156` — identical in the project's own 22.3.6 and in
  upstream), and the program ringbuffer is a **nested object**.
- The nested object's BOs are propagated to the submit only by
  `fd_ringbuffer_sp_emit_reloc_ring()`
  (`freedreno_ringbuffer_sp_reloc.h:94-108`), which walks
  `fd_target->u.reloc_bos`.

So the ISA BO's fate depends entirely on whether it is in that object's
`reloc_bos` list at propagation time. The measurement says it is not in the
submit. Whether it is lost in the object's `reloc_bos` list, or dropped at
propagation, is not yet distinguished.

Note the uapi is **not** implicated: `struct drm_msm_gem_submit_bo` and
`struct drm_msm_gem_submit_cmd` are byte-identical between the project's Mesa
22.3.6 header and the CAF kernel's, and the common prefix of
`struct drm_msm_gem_submit` matches. The kernel parsed `nr_bos=6 nr_cmds=1`
sensibly and every submitted BO had a correct IOVA and mmoff.

## What is not yet proven

That the dangling IOVA **is** the shader ISA's IOVA, rather than some other
unmapped pointer. The inference is strong — same dword offset in the shader
descriptor, in both render and compute; the ISA is known to be missing from the
submission; the address is in the correct IOVA range — but it is an inference,
and this project's own record is that eleven instruments have produced confident
wrong answers.

### The confirming step (small, additive, one flash)

Extend the existing DANGLING IOVA scan so that when it finds an unmapped
aperture address it **resolves that IOVA to the BO that owns it in the GPU
address space** and prints that BO's mmoff, size and cache flags.

- The GPU address space (`gpu->aspace`) holds every allocated IOVA, including
  BOs that were never submitted.
- If the dangling 0x700027000 resolves to a BO whose mmoff is 0x102179000 — the
  FS ISA mapping identified in (1) — the correspondence is **proven** and the
  root cause is closed.
- If it resolves to something else, the mechanism is different, and that is
  equally worth knowing.

This is a measurement added to an instrument that already exists; it does not
change the submit path.

## Harness work done this session

### `vmascan8.py` — fault-safe mapping scan (new)

`vmascan.py` … `vmascan6.py` read DRM mappings with a raw ctypes pointer
dereference:

```c
buf = (ctypes.c_ubyte * min(size, 4096)).from_address(start);
rawall = bytes(buf[:]);
```

which **SIGBUSes** (process dies, exit 135) on any mapping whose backing store
has been truncated — i.e. a BO that freedreno has already evicted from its cache.
`/proc/self/mem` does not help: drm-shmem pages are not in the page cache, so
`access_process_vm()` returns `-EIO` for **every** mapping (measured: 20/20).

`vmascan8.py` **forks per mapping**: the child reads and returns the bytes over a
pipe; if the child dies from SIGBUS the parent records "READ-FAULT" and moves on.
The child sees the same address space (COW), so the addresses are valid in it.
This is the only way to survive a hard fault in-process.

### `wrt2.py` — FD640 guard added

`wrt2.py` had **no renderer guard** and reported a false positive in this
session:

```
T1  VERDICT: GPU WROTE BUFFER MEMORY (compute->SSBO alive)
```

on `renderer=b'llvmpipe (LLVM 15.0.6, 128 bits)'` — the CPU software rasteriser
writing the buffer, because the GPU had not been brought up
(`get_param failed! -6`, `fd_pipe_new2: allocation failed`).

The guard now aborts with exit 3 unless `FD640` is in
`glGetString(GL_RENDERER)`. Verified in both directions: aborts on llvmpipe,
passes on FD640.

### A harness artifact that invalidated part of the earlier record

`vmascan6.py` documents, and this session confirms the significance of:

> Every previous harness in this project emitted a vertex shader that put ALL
> THREE vertices at the SAME position, i.e. a zero-area triangle … A zero-area
> triangle produces no fragments. If the draw is skipped before shader emission,
> `fd6_emit_shader()` never runs, `OUT_RELOC` is never called for the shader BO,
> and the BO never reaches the submit.

So image 259's "the shader ISA is never submitted" observation was made with a
harness whose draw could be dropped. It has been **re-measured here with a real,
non-degenerate triangle** (`gl_VertexID`-derived positions), and it **holds**:
the ISA is present in the Mesa process and absent from the submission. But note
that `vmascan7/vmascan8` (which use the real triangle) also have to get the draw
past the point of no return — the current harness does one `glDrawArrays` with
a real triangle and `glFinish` returns clean.

## Evidence

- `artifacts/gpu-bringup/evidence-270/vm8-out.txt` — the userspace mapping scan
  (VS/VS/FS magic hits, with file offsets)
- `artifacts/gpu-bringup/evidence-270/dmesg-vm8.txt` — the kernel submit dump
  for the same run (BO table + DANGLING IOVA)
- `artifacts/gpu-bringup/evidence-270/dmesg-wrt2-live.txt` — compute + TF +
  render submits, DANGLING IOVA across all of them
- `artifacts/gpu-bringup/evidence-270/wrt2-out.txt` — the workload verdicts
- `artifacts/gpu-bringup/harnesses/wrt2.py` — now with the FD640 guard
- `/home/usb/.opencode/vmascan8.py` — the fault-safe scan

## Next step

Image 271 = image 270 + **one additive change**: resolve the dangling IOVA to its
owning BO in the DANGLING scan and print that BO's mmoff/size. One flash, one
measurement, and the root cause is either proven or refuted.
