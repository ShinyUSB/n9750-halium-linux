# ROOT CAUSE: shader ISA buffers are never submitted to the GPU

Images 259 + harness `vmascan2.py`. This is the finding the project has been
circling for nineteen images.

## The measurement

Two magic constants are baked into the test shaders as `float` literals whose
bit patterns round-trip exactly:

```
MAGIC    = 0x3F123456  == 0.5711110830307007   (fragment shader)
MAGIC_VS = 0x3F0A0B0C                            (vertex shader)
```

After the draw, `vmascan2.py` walks `/proc/self/maps` **inside the Mesa
process** and finds both constants in shared mappings on `/dev/dri`, as
genuine a6xx ISA (2 dwords/instruction, incrementing instruction index in each
low dword):

```
0x7f94341000 4096 off=0x10217e000  0c0b0a3f00c04c20 0c0b0a3f01c04c20 …  <<< VS
0x7f94342000 4096 off=0x10217d000  0c0b0a3f00c04c20 0c0b0a3f01c04c20 …  <<< VS (binning)
0x7f94346000 4096 off=0x102179000  5634123f00c04c20 0000803e01c04c20 …  <<< FS
```

Mesa's own disassembly agrees on the content:

```
; FRAG  prog 1/1: 5 instr, 0 nops, 5 non-nops, 4 mov, 0 cov, 32 dwords
; VERT  prog 2/1: 1 instr, …
; BVERT prog 2/2: 1 instr, …
```

**So the shader ISAs are compiled, uploaded, and present in userspace memory.**

## The correlating measurement (image 259)

Image 259 prints the DRM fake **mmap offset** of every submitted buffer, which
is exactly the quantity `/proc/self/maps` reports. That reconciles the two views
for the first time:

```
submitted mmoffs: 0x10216a000 0x102177000 0x10217a000
                  0x10217b000 0x10217c000 0x102187000 0x10218c000

  0x10217e000  VS  -> *** NOT SUBMITTED ***
  0x10217d000  VS  -> *** NOT SUBMITTED ***
  0x102179000  FS  -> *** NOT SUBMITTED ***
```

Two submits captured (seq=3, seq=4), 6–7 buffers each, verified complete — no
shader ISA in any of them. The kernels never even attempt a decoded
`CP_LOAD_STATE6` parse; the offset join is layout-independent and cannot be
fooled by a misread PM4 header.

## Conclusion

**The command processor never receives a shader binary for any stage.** The VS,
the binning VS, and the FS are all compiled, uploaded and resident in userspace
— and none is submitted. The shader domain therefore has no program, which is
precisely the observed failure: commands execute and retire, nothing faults,
and no pixel is ever produced.

This also settles, retrospectively and correctly, the "empty shader ISA" thread:

- `bo[1]` (20480 B) and `bo[2]` (4096 B) were **never the shader ISA**. bo[1] is
  present in a clear-only run with no draw at all and does not change size from
  N=120 to N=1500 shader terms.
- The NUMAP-class buffers that did read `0/1024` were other objects entirely.
- The "empty ISA" was never empty and never submitted — it was never there.

## What remains open

Why the shader BO is dropped from the submission. The candidates, in order:

1. **The shader reloc is never emitted**, so the BO never enters the submit's
   BO array. The kernel's legacy path resolves relocs by index into a
   userspace-provided array, so a missing reloc means a missing buffer. Mesa
   reports non-zero `instrs_count` (5/1/1), so the `if (fs->instrlen)` guard in
   `fd6_emit_shader()` is *not* the gate — `instrlen` would be `ceil(1/16) = 1`.
2. **The BO is added but its IOVA is 0**, so freedreno omits it. `fd_bo_get_iova()`
   asserts `bo->iova != 0` in debug and emits 0 in release.
3. **freedreno's BO cache** returns a different object at submit time than at
   upload time — now the prime suspect, since the userspace object demonstrably
   holds the right bytes.

The discriminating next step is narrow: print, per submitted buffer, whether its
I reloc'd, plus the BO's `iova` and whether `so->instrlen` was non-zero when the
shader state was emitted. Everything else is now closed:

| area | status |
|---|---|
| kernel upload + submit path | **proven sound** (128 B write → 32/32) |
| userspace upload | **proven sound** (magic found in the VMA) |
| cache mode | **closed** (`bo_force_cached` A/B: `0x20000`→`0x10000`, no change) |
| disk shader cache | **closed** (deleted + disabled + `IR3_SHADER_DEBUG=nocache`) |
| mmap offset / wrong pages | **closed** (offsets join cleanly) |
| `glGetBufferSubData` as evidence | **retired** (GPU-side copy, unsound) |
| BO identity inference by size | **retired** (cost this project nineteen images) |

## Also worth recording

- The GPU **does** not come up on its own; `a6xx_gpu_load` is mandatory or Mesa
  silently falls back to llvmpipe, which renders a flawless `glClear`.
- `gbm_bo_get_fd` returns **-1** on this tree (no `struct dma_buf_ops`), so any
  oracle needing userspace to prefill a Mesa-allocated buffer is impossible.
  Kernel-side reads are the only sound view.
- Five device wedges this session; health must be checked via
  `a6xx_gpu_cpstate` before and after every GPU action, never `runtime_status`.