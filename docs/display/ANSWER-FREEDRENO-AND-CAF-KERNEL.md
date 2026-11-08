# Freedreno vs the CAF kernel: is this a Mesa/backend mismatch? — answered, with evidence

Date: 2026-10-20
Question: could it be that freedreno renders nothing because it doesn't know how to
handle this (CAF) kernel, and only works with the mainline msm driver?

**Answer: no.** The project already hit exactly this class of failure and fixed it.
The evidence below is from the live device, taken today.

## The strace (taken on the device, `strace -f -e raw=ioctl`)

```
      44 0xc0186443     DRM_MSM_GEM_INFO   (nr 0x43, size 24)
      23 0xc0106442     DRM_MSM_GEM_NEW    (nr 0x42, size 16)
      13 0xc0186440     DRM_MSM_GET_PARAM  (nr 0x40, size 24)
       2 0xc0486446     DRM_MSM_GEM_SUBMIT (nr 0x46, size 72)   <<<< SUBMITS ARE HAPPENING
```

The command numbers (`DRM_MSM_GEM_SUBMIT = 0x06`, etc.) are **identical to
upstream Linux** in the CAF tree (`msm_drm.h:443-461`), and the sizes match
upstream too: `drm_msm_gem_new` 16 B, `drm_msm_gem_info` 24 B,
`drm_msm_gem_submit` 72 B.

**Mesa issues `DRM_MSM_GEM_SUBMIT`.** So the "Mesa never submits" failure mode
is not present.

## The size-field mismatch is inert

The kernel's `struct drm_msm_gem_submit` is **40 bytes** (BTF:
`pahole -C drm_msm_gem_submit vmlinux`), while Mesa's is 72. The ioctl *number*
therefore differs (0xc0286442 vs 0xc0486442). **That does not matter**, because
the DRM dispatcher dispatches on the command number only:

```c
/* drm_ioctl.c:771,788 */
	unsigned int nr = DRM_IOCTL_NR(cmd);
	...
	unsigned int index = nr - DRM_COMMAND_BASE;
	ioctl = &dev->driver->ioctls[index];
```

and the copy is bounded by the *driver's* struct:

```c
/* drm_ioctl.c:802-834 */
	drv_size = _IOC_SIZE(ioctl->cmd);        /* the kernel's 40 */
	out_size = in_size = _IOC_SIZE(cmd);      /* the user's 72 */
	ksize = max(max(in_size, out_size), drv_size);
	...
	copy_from_user(kdata, (void __user *)arg, in_size);
```

Mesa's 72 bytes are copied, the kernel's handler reads the first 40 as the CAF
struct, the rest is ignored.

**And that the kernel reads it correctly is proven by the kernel's own
submit-time dump** — `BOS[seq=4] nr_bos=6 nr_cmds=1` with real IOVAs and real
mmoffs. For the kernel to see `nr_bos=6` at offset 8 and `bos` at offset 16,
Mesa must be writing the **CAF layout** (`flags` first) — i.e. the device runs the
project's CAF-patched Mesa, whose first 40 bytes are byte-identical to the
kernel's struct.

## The project already fixed this class of failure — image 187

`msm_drv.c:2281-2306`:

> **"TEMPORARY image 187 -- PROMOTE. The first change in this whole series that
> is an actual fix rather than a diagnostic.**
>
> DRIVER_SYNCOBJ was never set here, and that is why nothing has ever been
> submitted. drm_syncobj.c:499 refuses drm_syncobj_create_ioctl with
> -EOPNOTSUPP unless this feature bit is present, and drm_ioctl.c reports
> DRM_CAP_SYNCOBJ as 0. Mesa's DRI2 loader reads that, decides it cannot build a
> fence chain, and freedreno's fence_batch() has nowhere to signal completion --
> so tu_flush() never issues DRM_IOCTL_MSM_GEM_SUBMIT at all.
>
> Observed signature of exactly this, on #128:
>   strace: 76x MSM_GEM_INFO, 39x MSM_GEM_NEW, 9x MSM_GET_PARAM, and
>           ZERO GEM_SUBMIT and ZERO SYNCOBJ ioctls.
>   ring:    wptr_dw=99, CP_RB_RPTR == CP_RB_WPTR == 0x63 -- exactly where
>           the bring-up selftest left it, never appended to."

The current tree has `DRIVER_SYNCOBJ` (`msm_drv.c:2313`), and the strace above
shows `GEM_SUBMIT` being issued. **That failure mode is closed.**

## The related one: image 257

The same class of bug, one struct over: `drm_msm_gem_info` was a 16-byte CAF
struct while Mesa uses upstream's 24-byte one. Since the ioctl number encodes
`sizeof`, the kernel did not recognise Mesa's call at all. The tree's header
comment records it:

> *"So Mesa, which is built against the 24-byte upstream layout, issues a
> different ioctl number than this driver registered, and the kernel does not
> recognise the call at all."*

Image 257 restored the upstream 24-byte layout, and today's strace shows
`GEM_INFO` at size 24 ✓ and `GET_PARAM` at size 24 ✓.

## So the state of the UAPI question

| layer | state |
|---|---|
| command numbers (`DRM_MSM_*`) | identical to upstream ✓ |
| `drm_msm_gem_info` (24 B) | identical to upstream ✓ (image 257) |
| `drm_msm_gem_new` (16 B) | identical to upstream ✓ |
| `drm_msm_gem_submit` | kernel 40 B, Mesa 72 B — **harmless**: dispatch is by nr only, the copy is truncated to the driver's struct, and the first 40 bytes are the CAF layout ✓ |
| `DRIVER_SYNCOBJ` | set ✓ (image 187) — the "Mesa never submits" failure mode is closed |
| submit ioctl actually issued | **yes**, 2 per harness run ✓ |

**freedreno is talking to this kernel correctly.** The submission is provably
correct end to end (image 272: the `CP_LOAD_STATE6_FRAG` payload carries the FS
ISA's own IOVA, and the kernel sees the right ISA bytes in the right buffer).

## What that means for the "stages" question

The stages the project has already been through, and which are *not* the problem:

1. image 167 — 16-byte `gem_info` → Mesa's ioctls all rejected.
2. image 187 — `DRIVER_SYNCOBJ` missing → Mesa never issued GEM_SUBMIT at all.
3. image 257 — `gem_info` restored to upstream's 24 bytes.

The current state is *past* all of them. The remaining defect is not a
Mesa↔kernel mismatch, and not on the submission side at all: the command stream
is correct, the shader programs are delivered to the right addresses with the
right content, the page table is correct, the SMMU permits writes, the clocks
and power sequence match KGSL's, and the RB/SP still write nothing.

**So: the defect is inside the GPU, below the driver's ability to observe it** —
which is exactly why the project's remaining leads are all *platform* ones
(the EL2/HYP data abort, the a6xx trust configuration) rather than software ones.
