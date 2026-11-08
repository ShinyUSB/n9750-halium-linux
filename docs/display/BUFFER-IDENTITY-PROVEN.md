# Buffer identity — PROVEN. The buffer we read is the buffer the GPU writes.

## The question

`ORACLE-PHASE0-RESULTS.md` and everything since has rested on an unstated
assumption: that the dmabuf we `mmap` after `gbm_surface_lock_front_buffer()` is
the same buffer the GPU renders into.

This was worth doubting because:

- `EGL_EXT_image_dma_buf_import` is **ABSENT** on this stack (verified by
  `probe.py`: `dmabuf ABSENT`, `linux_dmabuf ABSENT`,
  `GL_OES_EGL_image_target_renderbuffer_storage ABSENT`). So the swapchain
  dmabuf is the only surface we can read back at all.
- The only instrument that could sidestep the swap (`glReadPixels`) is broken
  by the `pm_runtime` livelock.
- `IMAGE-244`'s `ibdump` shows **five or six BOs per submit**, with two of them
  carrying a distinct reloc flag (`0x6007` vs `0x6003`) and one alternating
  between two IOVAs — the behaviour of a rotating front buffer.

If the read buffer were the wrong one, every "the GPU does not write" conclusion
would be void.

## Method (`/root/idprobe.py`, host copy `/tmp/opencode/idprobe.py`)

No kernel change. Renders three frames, locks the front buffer, then dumps every
kernel-side identity for it *while holding it open* so the state stays valid:

1. userspace view — `gbm_bo` pointer, geometry, dmabuf fd/inode/size
2. `/sys/kernel/debug/dma_buf/bufinfo` — size, flags, count, inode, and the
   **exclusive fence state**
3. `/sys/kernel/debug/dri/0/gem` and `gem_names` — the kernel's BO table with
   IOVAs

The size filter does the correlation: our buffer is 64×64 RGBA8888 = **16384
bytes**, which is distinctive.

## Result

**Userspace:**
```
gbm_bo pointer : 0x208d5d10
geometry       : 64 x 64  stride=256  fmt=0x343252888
byte size      : 16384
dmabuf fd      : 7
dmabuf inode   : 37704
```

**Kernel, via `dma_buf/bufinfo` while held:**
```
size     flags  mode  count  exp_name  buf name  ino
00016384 00000002 00000007 00000003 drm  dmabuf6  37704  dmabuf6
	Exclusive fence: msm A640 signalled
	Attached Devices:
Total 0 devices attached
Total 1 objects, 16384 bytes
```

Three things fall out of that one line:

- **exactly one** 16384-byte dma-buf exists, and it is ours (`ino 37704`)
- its exclusive fence reads **`msm A640 signalled`** — the GPU has signalled
  completion **on our buffer**. The submit completed; the fence mechanism is
  live and correct.
- `Attached Devices: Total 0` — normal, the buffer is unmapped while locked for
  CPU read, as expected.

**Kernel BO table, queried during the hold:**
```
$ awk '{ if ($NF==16384) print }' /sys/kernel/debug/dri/0/gem
   00020001: I  0 ( 2) 00000000 0000000000000000   700025000 16384
```

Exactly one BO of our size, at IOVA **`0x700025000`**.

**And the ibdump agrees:**
```
BOS[seq=28] bo[0] iova=0x700025000 flags=0x6003 bo=ffffffc341d1da00
```

Same IOVA, `ffffffc341d1da00`. The buffer we read, the BO the kernel holds, and
the BO the GPU was handed are the same object.

## Conclusion

**The chain is closed:**

```
gbm front buffer (ino 37704, 16384 B)
  == dma_buf/bufinfo dmabuf6 (16384 B, ino 37704, fence: msm A640 signalled)
  == dri/0/gem object at IOVA 0x700025000
  == ibdump BOS[] bo[0] iova=0x700025000
```

So:

1. We are reading the right buffer.
2. The GPU signals a fence on it — completion works.
3. Yet after that fence, the buffer is all zeros on a sound readback.

That eliminates "wrong buffer" as an explanation for the entire corpus, and it
eliminates the swap/resolve-blit theory as well: `bo[0]` is a 16384-byte
*colour* buffer at a known IOVA, and it is the buffer the GPU is handed directly
for the clear.

This also retires the observation that looked most alarming in IMAGE-244 — the
`0x6007` BOs alternating between `0x700029000` and `0x70002a000`. Those are the
**other** swapchain buffers (the `flags` bit distinguishes scanout-capable
objects); our locked one is the `0x6003` object at `0x700025000`. The rotation
is normal triple-buffering, not a red herring pointing at a mismatched buffer.

The `0x700015000`–`0x70002b000` cluster is simply a set of 16 KB swapchain
buffers plus command streams in one aperture region. Nothing anomalous.

## What this leaves

Every remaining live hypothesis is about the GPU not *performing* the store, not
about us reading the wrong memory:

- **zap / secure mode** — firmware extracted (`GPU-ZAP-FIRMWARE.md`), needs DT +
  driver work. Caveat stands: the fence echo works, so CP non-secure DDR writes
  land; a blanket secure-mode stall does not explain CP-writes/RB-doesn't.
- **`glReadPixels`** — now confirmed as the only other way to observe the render
  independently of the swap. Fixing the `pm_runtime` livelock would let us
  render to an FBO and read that directly, isolating "render broken" from
  "present-to-swapchain broken". Given identity is now proven, these are the
  same question, and this is the cheapest way to answer it.