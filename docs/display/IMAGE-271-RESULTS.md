# Image 271 — results: the IOVA resolver works, and it corrects two of my claims

Date: 2026-10-18
Image: 271 (base 270 + the PTU271 IOVA resolver)
Evidence: `artifacts/gpu-bringup/evidence-271/` — `dmesg-271.txt`, `phase1.txt`,
`phase2.txt`, `dmesg-freeze.txt`

## 1. The resolver works, and the device survived

```
PTU271[resolve] iova 0x700027000 (bo[3] dword 36) OWNED BY bo:
    iova=0x700026000 size=0x5000 mmoff=0x102189000 objsz=20480 boflags=0x20000 name='' off=0x1000
PTU271[resolve] iova 0x700021010 (bo[11] dword 280) OWNED BY bo:
    iova=0x700021000 size=0x1000 mmoff=0x102175000 objsz=4096 boflags=0x20000 name='' off=0x10
```

No oops (`grep -c 'Unable to handle|Internal error'` = 0), and the device was still
alive and reachable afterwards. Both phases completed:
phase 1 with `ibdump=0` (survival test) and phase 2 with `ibdump=1`.

## 2. CORRECTION — the "DANGLING IOVA" hits are false positives

The image-260 DANGLING scan uses **exact IOVA equality**, not containment, so it
flags every *interior* pointer into a submitted buffer as "dangling". The
resolver proves it:

| reported as dangling | actually |
|---|---|
| `0x700027000` | offset **0x1000 inside** the submitted 20480-byte BO at 0x700026000 |
| `0x700021010` | offset **0x10 inside** the submitted 4096-byte BO at 0x700021000 |
| `0x700000007` | offset 0x7 inside a submitted BO (inactive list) |

**None of them is an unmapped address.**

This instrument is already on the project's list of things that produced
confident wrong answers:

> "the `DANGLING` scan (exact IOVA equality instead of containment)"
> — `STATE-END-OF-SESSION-2026-11-16.md`, "Instrumentation count"

I rebuilt on it anyway. That was the mistake, and the resolver is what caught
it. **The scan also has a second, larger gap: it only reports addresses that are
NOT in the submitted set. It never says which BO a *matched* address points
into.** So if the shader descriptor's ISA pointer is exactly a submitted BO's
IOVA, this instrument is silent about it. See "Next step".

## 3. What survives, and what is now the sharpest fact

**Survives (it rested on the mmoff comparison, not on the DANGLING scan):**
the FS and VS ISA buffers are present in the Mesa process and are **not** in the
submission.

Same run, joined by mmoff between the userspace map and the kernel's submit
table:

```
userspace /dev/dri mappings with the magic constant inside real a6xx ISA:
   mmoff=0x102179000 (FS MAGIC)   mmoff=0x10217a000 (VS MAGIC)   mmoff=0x10217b000 (VS MAGIC)

submitted BO table, seq=4:
   bo[0] iova=0x700022000 mmoff=0x102176000 sz=4096
   bo[1] iova=0x700026000 mmoff=0x102189000 sz=20480   <- NOT mapped in the Mesa process
   bo[2] iova=0x700039000 mmoff=0x10218e000 sz=4096    (userspace_nz = 0, all zero)
   bo[3] iova=0x70003b000 mmoff=0x102188000 sz=4096
   bo[4] iova=0x700015000 mmoff=0x102169000 sz=4096
   bo[5] iova=0x70003a000 mmoff=0x102187000 sz=4096
```

**None of the three magic-bearing ISA mappings is in the submitted set.**

**The new sharpest fact:** the 20480-byte buffer that the descriptor points into
(bo[1], mmoff 0x102189000) is **entirely zero in the kernel's view** — 0 of 5120
dwords nonzero, including at the +0x1000 offset the descriptor names. And
**no submitted BO contains the FS or VS magic constant at all.**

So the GPU is pointed at a program address whose memory is empty, while the
compiled ISA sits in a buffer the submission does not carry.

Two readings remain, and they are different defects:
- (a) the 20480-byte buffer is where the ISA is *supposed* to be, and the upload
  never landed there; or
- (b) the 20480-byte buffer is something else, and the descriptor's real ISA
  pointer is at a different dword that the broken scan never reported.

**Reading (b) is now the more likely one.** Size and flags fit freedreno's
private-memory buffer exactly: `fd6_program.c:117-120` creates
`ctx->pvtmem[...].bo` as `fd_bo_new(dev, total_size, FD_BO_NOMAP, "pvtmem_%s_%d", ...)`
with `total_size = ALIGN(per_fiber_size, 1<<12) * num_sp_cores`; for a640
`num_sp_cores` is 4, and 20480/4 = 5120 = `per_fiber_size`. The buffer is
`FD_BO_NOMAP` (not mapped in the Mesa process — as observed) and WC — as
observed. If so, the descriptor's dword 36 is the **private-memory address**, not
the program address, and the ISA pointer is elsewhere in the descriptor.

## 4. What caused the freeze

The device reset with:

```
UploadCause[PMIC abnormal reset]
collect_rr_data : restart_reason = 0x776655ee
```

— a PMIC (power IC) reset, **not** a kernel panic. The archived PMIC-reset
captures in `artifacts/gpu-bringup/crashes/` all show:

```
Workqueue: pm pm_runtime_work
```

i.e. a **power-management** context, consistent with the GPU power-domain
bring-up rather than with the driver.

**The 300 s rule is necessary and I had been violating it.** This session's
first two attempts brought the GPU up at ~50 s and at ~20 s uptime; both froze.
The run that brought it up at **329 s** completed the whole ibdump exercise and
left the device alive.

It is not sufficient, though: an earlier attempt that did respect the rule still
froze. `PMIC abnormal reset` appears **8 times in the archive** (vs 117
"Non Secure Watchdog Bark" and 54 "KERNEL PANIC"), so it is a known,
intermittent, pre-existing hazard of this platform — most likely the GPU power
sequence — and it is not something the 271 patch introduced.

## 5. New negative result: buffer names are not available with this Mesa

The kernel implements `MSM_INFO_SET_NAME` (`msm_drv.c:1723`) and the ioctl
struct is byte-identical to upstream's, and `MSM_INFO_GET_IOVA` demonstrably
works (correct IOVAs come back). So why is every `name=''`?

Because the device's Mesa never calls it. Debian's stock Mesa
**22.3.6-1+deb12u2** has **no buffer-naming format strings at all**:

```
$ strings /usr/lib/aarch64-linux-gnu/dri/msm_dri.so | grep -E '^(fs|vs|pvtmem|cmdstream|gmem):'
   (nothing)
$ strings ... | grep -E 'pvtmem|cmdstream'
   (nothing)
```

So `fd_bo_new()` in this Mesa passes no name, `msm_bo_set_name()` is never
invoked, and the kernel's `name[64]` stays empty. **The tree's image-258 comment
— "freedreno labels every buffer it creates: 'fs:...', 'vs:...',
'pvtmem_per_fiber', 'cmdstream', 'gmem:...'" — is true of newer Mesa, not of
this one.** Name-based buffer identification needs a Mesa rebuild; it is not
available on the current image.

## 6. Next step

**Image 272: turn the DANGLING scan into a POINTER MAP.**

For every 64-bit value in every submitted buffer, in the aperture range, print
the value **and the buffer it lands in** — whether or not it is "dangling",
using **containment** rather than exact equality:

```
PTRMAP[seq=N] bo[3] dword 36 -> iova 0x700027000 -> BO mmoff=0x102189000 sz=20480 off=0x1000
PTRMAP[seq=N] bo[3] dword 52 -> iova 0x70002a000 -> BO mmoff=0x102179000 sz=4096  off=0
```

One dump then shows **every GPU address the command stream references and which
buffer each one lands in**. That single dump answers the open question:

- the descriptor's ISA pointer lands in the 4096-byte **ISA buffer**
  (mmoff 0x102179000) → the ISA *is* submitted and the whole "ISA missing" line
  of reasoning collapses; the fault is downstream of that;
- it lands in a buffer that is **not** in the submission → the ISA is genuinely
  not submitted, and the mechanism is Mesa's;
- it lands in the **all-zero** buffer → the program the GPU is told to run is
  empty, and the fault is the upload.

The resolver (`a6xx_resolve_dangling_iova()`) already does the "which BO is
this" part and is proven; it only needs to be called for *every* aperture
pointer, not just the unmatched ones, and the containment test fixed.

## 7. Operational notes (hard-won this session)

- **The 300 s rule.** `a6xx.gpu_init_delay_ms=300000`: writing `1` to
  `a6xx_gpu_load` before uptime > 300 s freezes the device. It is now in
  `CLEAN-SESSION-RECIPE.md`, which previously omitted it.
- **A file on the phone is not evidence.** After the unclean reset, `/userdata`
  rolled back to a pre-run state and the entire measurement was lost —
  `run271.log` came back empty and `dmesg-live.txt` turned out to be the
  *previous* boot's file. The device now **pushes its evidence to the host over
  RNDIS** (`/home/usb/.opencode/collect.py` + `run271.sh`), so a freeze cannot
  destroy it.
- **The `ibdump` instrument is not what wedges the device.** A full ibdump run
  (11 706 printk lines) completed with the device alive, once the 300 s rule was
  respected.
- `poller.sh` writes to `/root/cpsamples.txt` on the 100 %-full `/`, and its
  `cut`/`cat` loop floods dmesg with Samsung's `five-diag:` lines, evicting the
  very evidence being collected.
