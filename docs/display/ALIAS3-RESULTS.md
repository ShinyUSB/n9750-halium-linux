# Aliasing test v3 — userspace buffer uploads DO work; two more instruments retired

Redone after image 258, per "ok, redo". Two harness generations, both designed
so they *can* fail, plus one that initially appeared to succeed and did not.

## 1. v2 (uniform buffer) — self-check correctly FAILED

`alias2.py` used two UBOs with distinctive page-aligned sizes (24576 B = 6144
dwords, 16384 B = 4096 dwords) and required the kernel BOS dump to contain those
sizes before drawing any conclusion:

```
distinct sizes in bos[]: [4096, 20480]
SELF-CHECK bufA (24576B/6144dw) submitted? False
SELF-CHECK bufB (16384B/4096dw) submitted? False
```

**freedreno/a6xx does not submit uniform buffers as BOs in this configuration.**
So a UBO probe cannot observe anything, no matter what it concludes. The test
is void — and it said so itself, which is the entire reason for writing the
self-check. Contrast with `pattern.py`, which reported "invisible" for buffers
it never submitted.

Side observation: post-draw `glGetBufferSubData` returned zeros for both UBOs.
That turned out to be the artifact described in §3, not a second failure.

## 2. v3 (vertex buffer) — first attempt was culled

First run filled the vertex buffer with a repeated `0xA5A5A5A5` and used
`gl_Position = a`. As a float that is ±1.3e38 in every component, so every
vertex is far outside the clip volume and the draw is trivially culled — nothing
is attached. Switched to real positions (`x,y,z,w = 0,0,0,1` → NDC (0,0,0),
the centre of the screen).

Even so the 24576-byte VB never appears in `bos[]`. Command-stream content does
change with it (`bo[3]`'s first dword moves `0x20000` → `0x22000`), so the draw
does reference something. Left unexplained, and deliberately not over-claimed.

## 3. THE RESULT: uploads land correctly, and glGetBufferSubData is unsound

Reading the freshly-filled vertex buffer back, pre-draw, by two routes:

```
PRE-DRAW glGetBufferSubData : all zeros, 0x3f800000 hits=0/16
PRE-DRAW map(READ) direct   : 0x00000000 0x00000000 0x00000000 0x3f800000  ...  hits=4/16
PRE-DRAW VERDICT: data IS in the BO pages
```

`glMapBufferRange(GL_MAP_READ_BIT)` is a direct CPU mapping with no GPU in the
loop, and it returns the pattern **exactly as written** — every 4th dword
`0x3f800000`, 4 hits in 16, first eight dwords precisely `0,0,0,0x3f800000,0,0,0,0x3f800000`.

**Therefore: userspace buffer uploads work.** `glBufferData` → transfer →
`fd_bo_upload` → `bo_map` → mmap → memcpy puts the data in the BO's real pages.

**`glGetBufferSubData` is not a sound readback on this device.** For a
GPU-visible buffer, freedreno satisfies it with a GPU-side copy into staging;
since the GPU cannot write, staging comes back zero regardless of what is
really in the BO. It reports zeros even when the data is verifiably present.

This retires, cleanly:

| claim | status |
|---|---|
| userspace writes go to pages the GPU never sees | **refuted** for ordinary buffers — data is in the BO pages |
| GEM/mmap aliasing bug | **closed** — the direct CPU mapping reads the correct physical pages |
| the whole "kernel view vs userspace view" family | **closed**; both read the same, correct, memory |
| `glGetBufferSubData` as a verification tool | **retired as unsound** |
| `bo_force_cached` / writecombine as a cause | already refuted in image 258 (all BOs are `MSM_BO_WC`) |

I was one step from repeating the exact error this session has been correcting:
the intermediate run of `alias3.py` printed "BO IS ZERO -- the upload never
landed" and that verdict was wrong, for exactly the reason `pattern.py` was
wrong before it.

## 4. What is actually left

The shader ISA buffers remain empty (`bo[1]` 20480 B = 0/5120, `bo[2]` 4096 B =
0/1024) — with the cache deleted, `MESA_SHADER_CACHE_DISABLE` +
`MESA_GLSL_CACHE_DISABLE`, cold compile, FD640 confirmed, PM held.

But the mechanism is now known to work. The vertex buffer goes through the
*same* `fd_bo_upload()` → `bo_map()` → mmap → memcpy that the shader uses:

```c
/* vertex buffer */                          /* shader ISA */
glBufferData -> transfer -> fd_bo_upload(bo, ptr, ...)   upload_shader_variant:
                                                    fd_bo_upload(v->bo, v->bin, 0, v->info.size);
```

So the difference cannot be the upload mechanism. An all-zero destination with
a memcpy that provably runs means the **source** is zeros: `v->bin` is empty or
zero-filled at copy time.

That sharpens the question to something specific and testable in Mesa, not the
kernel: is `v->bin` populated when `upload_shader_variant()` runs, and is
`v->info.size` the assembled ISA length or an alignment-padded BO size? Note the
4 KB VS BO against a trivial `gl_Position = vec4(0,0,0,1)` shader — if the real
ISA is ~128 bytes, "0 nonzero dwords" may partly be padding, and the subagent's
H2 (bo[1]/bo[2] being state objects rather than the ISA) is still open.

`IR3_DEBUG=ir` prints nothing in this Mesa build, so the ISA length was not
obtained. The next useful step is to get the assembled ISA out of Mesa — its
length, and its first bytes — and compare that against the kernel's view of
`bo[1]`/`bo[2]`. That is a Mesa-side question and does not need a flash.