# BREAKTHROUGH: the shader ISA IS present in userspace memory

Date: 2026-10-15. Harness: `harnesses/vmascan.py`.

## The result

`vmascan.py` compiles a fragment shader whose GLSL literal round-trips to the
float `0.5711110830307007` (bit pattern `0x3F123456`), draws with it, then walks
`/proc/self/maps` **inside the Mesa process** and searches every shared mapping
whose backing file is `/dev/dri/`.

```
range              size  fileoffset  nonzero  first 32B
0x007f845c5000   4096   0x10217e000   8       5634123f00c04c200000803e01c04c200000003f02c04c200000803f03c04c20   <<< MAGIC FOUND
```

Decoded as a6xx ISA (2 dwords per instruction):

```
3f123456 00c04c20     <- our magic as the operand of instruction 0
0000803e 01c04c20
0000003f 02c04c20
0000803f 03c04c20
```

The low dwords carry an incrementing instruction index (0,1,2,3), so these are
genuine instructions, and the count matches what Mesa itself reported:

```
; FRAG prog 1/1: 5 instr, 0 nops, 5 non-nops, 4 mov, 0 cov, 32 dwords
```

## What this settles

**`fd_bo_upload()` → `bo_map()` → `mmap` → `memcpy` WORKS.** The ISA is
materialised in the Mesa process's address space, at a 4096-byte DRM mapping.
This closes, permanently and by direct observation:

- the "empty shader ISA" as a Mesa-side compile failure (Mesa compiled it);
- the "the memcpy writes the wrong physical pages" theory (it wrote the right
  ones, and the bytes are there);
- every remaining *userspace* explanation.

Together with the kernel-side proofs — a 128-byte memcpy into a 4096-byte
writecombine BO submitted by raw ctypes reads back **32/32** in the kernel's own
`a6xx_submit()` dump — the write mechanism is correct on **both** sides.

## What it leaves

The ISA exists in userspace memory, but the kernel's BOS dump has never shown
ISA content: the FD_BO_NOMAP-class buffers appear as `0/1024` nonzero, and the
submission lists only 6–7 buffers while the process holds **20** shared DRM
mappings, several of which contain 5–8 nonzero dwords of instruction-like data.

So the open question is now sharply about **buffer identity**, not about writing:

> Is the userspace mapping at file offset `0x10217e000` the same object as the
> buffer the kernel submits? Or are they two different buffers, one of which
> the GPU is handed and the other of which Mesa filled?

The ISA's userspace mapping has DRM fake **mmap offset** `0x10217e000`. The BOS
dump prints the buffer's **IOVA**, never its mmap offset. That is precisely the
missing link, and closing it is a one-line diagnostic.

## Proposed next image — print the mmap offset in the BOS dump

In `a6xx_submit()`'s BOS header line, add `msm_gem_mmap_offset(bo)` alongside
`iova` and `bo->flags`. Then:

- match the ISA VMA (offset `0x10217e000`, in this run) against the submitted
  buffers by offset;
- **offset appears in bos[]** → identity confirmed; the BO is submitted but the
  kernel reads different bytes than userspace wrote, which contradicts the
  proven 32/32 round-trip and points at page detachment between upload and
  submit;
- **offset does not appear** → the shader BO is never submitted at all, and the
  bug is that the draw references a buffer the submission does not carry. That
  is a single, very specific defect and would also explain the empty ISA
  "forever".

This is the smallest experiment that distinguishes the last two worlds, needs
no userspace change, and — unlike hand-crafted PM4 or a cpu-prefill oracle —
cannot produce a false negative from a misparsed packet.

## Secondary finding from the same run

`/dev/dri/` had 20 shared mappings. Several 4096-byte ones contained 5–8
nonzero dwords of plausible instruction data. With no `dma_buf_ops` on this
tree, userspace cannot export BOs, so the VMA list plus the kernel dump are the
only two views available; reconciling them by mmap offset is the way forward.

## Standing hazards

This run ended with the device frozen again (fifth wedge across the session).
`vmascan.py` itself is read-only w.r.t. the GPU and completed cleanly on its own;
the freeze came around the `ibdump` invocation. Health must be checked via
`a6xx_gpu_cpstate` before and after every GPU action.