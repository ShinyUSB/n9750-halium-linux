# 🎉 Image 190 — Mesa finally submits. And 191 is queued behind one flag.

Kernel `#132`. This is the image where userspace stopped fighting us and
started actually talking to the GPU.

## 190 worked

**1. The crash is gone.** The staged harness is the proof, because on `#131`
stage 6 produced exactly one kernel fault every single run:

| stage | `#131` faults | `#132` faults |
|---|---|---|
| S1–S5 (EGL, context, FBO, program) | 0 | 0 |
| **S6 (glClear + glFlush)** | **1** | **0** |

`BLR x4` with `x4 == 0` at `msm_dri.so + 0x9db548` is gone, by advertising
`MSM_VERSION_MINOR = 4` so Mesa selects `sp_funcs` (which has a `.flush`)
instead of `legacy_funcs` (which has none). Record:
`ROOT-CAUSE-DRM-VERSION.md`.

**2. Mesa opened a submitqueue and issued a real submit.** First time ever in
this project:

```
1287  ioctl(5, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, 0x7ff6a67b70) = 0
1288  ioctl(5, DRM_IOCTL_ETNAVIV_GEM_SUBMIT or DRM_IOCTL_MSM_GEM_SUBMIT,
           0x7f90bbe728) = -1 EINVAL (Invalid argument)
```

`GEM_SUBMIT=1`, `SUBMITQUEUE=2` out of 143 ioctls. For 189 images it was
zero. Mesa is now genuinely trying to give the GPU work.

## And the driver told us exactly why it refused

```
[drm:msm_ioctl_gem_submit] *ERROR* invalid flags: 7
```

Not a crash, not a hang — a precise, actionable rejection from
`msm_gem_submit.c:113`:

```c
if ((submit_bo.flags & ~MSM_SUBMIT_BO_FLAGS) ||
    !(submit_bo.flags & MSM_SUBMIT_BO_FLAGS)) {
        DRM_ERROR("invalid flags: %x\n", submit_bo.flags);
        ret = -EINVAL;
```

## 191: `MSM_SUBMIT_BO_DUMP`

Mesa sent `7`. This CAF driver only knows `3`:

| | READ | WRITE | DUMP | MSM_SUBMIT_BO_FLAGS |
|---|---|---|---|---|
| **Mesa 22.3** | 0x1 | 0x2 | **0x4** | READ\|WRITE\|DUMP = 7 |
| **our kernel** | 0x1 | 0x2 | **missing** | READ\|WRITE = 3 |

`7 & ~3 = 4` → EINVAL. A pure UAPI drift: this CAF driver predates
`MSM_SUBMIT_BO_DUMP`, which upstream added as a debugging hint ("dump this
BO's contents", used by Mesa's Vulkan path and its commandstream replay tool).

Image 191 adds `MSM_SUBMIT_BO_DUMP 0x0004` to the uapi header and to
`MSM_SUBMIT_BO_FLAGS`. The driver ignores it, which is correct for a debug
flag we never use, and it does not collide with internal flags (`BO_VALID` is
`0x8000`).

## How close this is

Two images ago the GPU had never executed a command we submitted. Now:

- driver submit path: **proven working** (image 189, `wptr` 99→114, IRQ 0→3)
- Mesa issuing submits: **achieved** (image 190)
- Mesa's submits being *accepted*: **one flag away** (image 191)

There is still an honest unknown: `wptr_dw` is still 99 on `#132` because the
submit was rejected before it reached the ring. Whether 191 produces an
accepted submit that then executes — and whether the IRQ completes, which has
only ever been seen once via a hand-built submit — is unproven. But the
distance between "Mesa tries" and "Mesa's work reaches the GPU" is now one
constant, and every remaining step is something we can read in a log rather
than infer.

Phone alive on `#132`, GPU loaded, reachable over USB RNDIS.