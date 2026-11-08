# Image 272 — the root cause, proven: the shader program is never delivered to the GPU

Date: 2026-10-18
Images: 270 (page-table prologue), 271 (IOVA resolver), 272 (pointer map)
Evidence: `artifacts/gpu-bringup/evidence-272/` — `phase1.txt`, `phase2.txt`,
`vm9.txt`, `dmesg-272.txt`

## Verdict

**The GPU is told to fetch the shader program from an empty buffer.** The a6xx
shader descriptor in the command stream points at offset 0x1000 of a submitted
20480-byte arena; that arena is **entirely zero in the kernel's view**. The
real compiled ISA — vertex, binning vertex and fragment — lives in three
*different* buffers, at IOVAs the command stream **never mentions** and the
submission **never carries**.

That is the whole defect, and it explains every symptom the project has been
chasing: the RB and the SP both die (both need the program), the CP keeps
writing to DDR (it uses a different, non-empty buffer), Mesa's state is live in
the registers (the CP applies it), and nothing ever faults (fetching zeros is
not a fault).

## The measurement, and how the three views were joined

Three views had to be joined, and each join is by a value that means the same
thing on both sides:

| view | what it gives | join key |
|---|---|---|
| userspace `/proc/self/maps` | the ISA is present and correct | DRM fake offset (file offset) |
| kernel `submit->bos[]` | what the submission carries | `msm_gem_mmap_offset()` |
| kernel debugfs `/sys/kernel/debug/dri/0/gem` | every BO's own IOVA | `off` (in **page** units → `<< 12`) |

The debugfs BO dump (`msm_gem.c:1149,1169,1174`) prints per BO the flags,
active/inactive, the mmap offset and every VMA's IOVA. It had never been used
for this. `vmascan9.py` (new, harness-only — no kernel change) reads it from
inside the Mesa process while that process's BOs still exist.

### 1. The ISA is in the Mesa process, and correct

```
mmoff=0x102179000  size=4096  nz=8   <<< VS MAGIC
mmoff=0x10217a000  size=4096  nz=8   <<< FS MAGIC
mmoff=0x10217b000  size=4096  nz=8   <<< VS MAGIC
```

The magic constants (`0x3F123456` in the FS, `0x3F0A0B0C` in the VS) are baked
into the shaders and appear verbatim inside the compiled a6xx ISA, so these
mappings are unambiguously the shader programs, and they are populated.

### 2. The ISA buffers have their own IOVAs, and the stream never mentions them

From the kernel's own BO table:

```
ISA mmoff=0x102179000  own_iova=0x70002b000   referenced by the stream: NO
ISA mmoff=0x10217a000  own_iova=0x70002c000   referenced by the stream: NO
ISA mmoff=0x10217b000  own_iova=0x70002d000   referenced by the stream: NO
```

and none of them is in the submission's BO list.

The pointer map (image 272) lists **every** GPU address the command stream
references, with the buffer it lands in — by containment, at every dword
alignment, not just the ones that fail to resolve:

```
PTRMAP[seq=4] bo[3] dword 2   -> iova 0x700021000 -> BO mmoff=0x102175000 off=0x0
PTRMAP[seq=4] bo[3] dword 36  -> iova 0x700027000 -> BO mmoff=0x102189000 base=0x700026000 size=0x5000 off=0x1000
PTRMAP[seq=4] bo[3] dword 45  -> iova 0x700026000 -> BO mmoff=0x102189000 base=0x700026000 size=0x5000 off=0x0
PTRMAP[seq=4] bo[3] dword 53  -> iova 0x700039000 -> BO mmoff=0x10218e000 off=0x0
PTRMAP[seq=4] bo[3] dword 74  -> iova 0x700000007 -> BO mmoff=0x10218f000 off=0x7
```

Ten distinct buffers are referenced. **None is a shader ISA buffer.** The
descriptor's two interesting pointers are dword 36 and dword 45, and both land
in the same 20480-byte buffer (its own IOVA 0x700026000, size 0x5000): the base
and base+0x1000.

Note the geometry: the arena spans 0x700026000–0x70002b000, and the three ISA
buffers are allocated immediately after it (0x70002b000, 0x70002c000,
0x70002d000). So the program address the GPU is given is *inside the arena*,
while the programs themselves are *just past it*.

### 3. The arena the GPU is pointed at is empty

The kernel's own content dump of the 20480-byte buffer, in the same submit:

```
bo[1] mmoff=0x102189000 sz=20480  dumped=5120  nonzero=0
```

**Zero nonzero dwords out of 5120** — including at the +0x1000 offset the
descriptor names. And **no submitted BO contains the FS or VS magic constant
at all** (checked across every dword of every submitted buffer).

The arena is also `FD_BO_NOMAP`: it has no userspace mapping (it does not
appear in the Mesa process's `/proc/self/maps`), so freedreno cannot have
written it through a userspace mapping.

## Why this explains every symptom

| symptom | explanation |
|---|---|
| RB → DDR writes never observed | the render backend has no program to execute |
| SP (compute) → DDR writes never observed | same, for the compute shader |
| the CP still writes DDR (`memptrs->fence` increments) | the CP's own stores go to a different, non-empty buffer |
| Mesa's state is live and workload-tracking in the aperture registers | the CP applies state; the shader never runs |
| the SQE executes `CP_DRAW` and `CP_LOAD_STATE6_FRAG` | the stream is consumed; the program it names is empty |
| no SMMU or GPU fault ever raised | fetching zeros is not a fault |
| a valid page table is installed (image 270) | translation is fine; the mapping is *empty* |
| the DANGLING IOVA scan fired | it was flagging interior pointers, not unmapped memory (see below) |

## Two of my own errors, corrected by this work

1. **The DANGLING IOVA scan was broken and I rebuilt on it.** It used exact
   IOVA equality instead of containment, so every interior pointer into a
   submitted buffer looked unmapped, and it only ever reported the addresses
   that did *not* resolve. The project had already listed it among the eleven
   instruments that produced confident wrong answers
   (`STATE-END-OF-SESSION-2026-11-16.md`). Image 271's resolver proved the
   false positives; image 272 replaced the scan with a pointer map.

2. **The "20480-byte buffer is an all-zero buffer the descriptor points into"
   reading was right, but my guess that it was `pvtmem_per_fiber` was not
   needed** — and the ISA buffers' own IOVAs now show the program address is
   *inside* the arena while the programs are *just past* it. The geometry
   alone pins it down.

## What remains open

**Why the arena is empty.** The a6xx program layout evidently places the ISA at
arena+0x1000; something must copy each stage's compiled ISA there, and on this
tree it does not happen — or the arena is not the program arena at all and the
descriptor's pointer is simply wrong. The pointer map makes this a
source-reading question in freedreno's a6xx program path rather than a
measurement question, which is the right place for it to be.

Useful constraints for that reading:
- the descriptor entries sit at dwords 33-38 of the command stream, with the
  program address at 36 and the arena base at 45;
- the arena is 0x5000 bytes and `FD_BO_NOMAP`;
- the three per-stage ISA buffers are allocated immediately after it and are
  never referenced or submitted.

## Instrument changes this session

| file | change |
|---|---|
| `drivers/gpu/drm/msm/adreno/a6xx_gpu.c` | image 271: IOVA resolver; image 272: POINTER MAP replacing the DANGLING scan |
| `harnesses/wrt2.py` | FD640 guard added — it had reported a false positive on llvmpipe |
| `harnesses/vmascan8.py` | fork-per-mapping, so a SIGBUS on a truncated DRM mapping no longer kills the scan |
| `harnesses/vmascan9.py` | new: reads the kernel's debugfs BO table from inside the Mesa process |

## Operational notes (all hard-won)

- **The 300 s rule.** `a6xx.gpu_init_delay_ms=300000`: writing `1` to
  `a6xx_gpu_load` before uptime > 300 s **freezes the device**. Now in
  `CLEAN-SESSION-RECIPE.md`.
- **A file on the phone is not evidence.** After an unclean reset `/userdata`
  rolls back to a pre-run state; a whole measurement was lost this way. The
  device now pushes its evidence to the host over RNDIS.
- **The freeze signature is `UploadCause[PMIC abnormal reset]`**, with
  `Workqueue: pm pm_runtime_work` in the archived captures — a power-management
  context, i.e. the GPU power-domain bring-up. 8 archived instances, so it is
  pre-existing and intermittent.
- **`ibdump` is not what wedges the device** — a full 11 706-line run completed
  with the device alive, once the 300 s rule was respected.
- **`/sys/kernel/debug/dri/0/gem`** prints, per BO, the mmap offset (in page
  units — multiply by 4096) and every VMA's IOVA. It is the cheapest
  available join between the kernel's and the userspace's view of a buffer.
- **Debian Mesa 22.3.6 never calls `MSM_INFO_SET_NAME`** (no naming format
  strings in `msm_dri.so`), so `name=''` is permanent with this Mesa. The
  tree's image-258 comment claiming freedreno labels buffers describes newer
  Mesa.
