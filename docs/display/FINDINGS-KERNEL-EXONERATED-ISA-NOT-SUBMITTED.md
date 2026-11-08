> ## SUPERSEDED AND PARTLY RETRACTED — read
> `FINDINGS-KERNEL-EXONERATED-SHARP-PROBLEM.md` instead.
>
> Two claims below are wrong:
>
> 1. **"The draw is never emitted" — RETRACTED.** The A/B actually showed a
>    clear-only command stream with 240 nonzero dwords versus 244 with the draw.
>    The draw *is* emitted; I misread identical BO sets as "nothing happened".
>    I then tried to decode `CP_LOAD_STATE6` to confirm identity, guessed the
>    header bit layout, and got it wrong twice — the same class of error that
>    produced tonight's other false claims.
> 2. **"`bo[1]`=20480 is a fixed-size buffer, not the shader ISA" — CORRECT and
>    load-bearing**, and it is why most of section 3 below is about the wrong
>    object.
>
> What survives: the kernel exoneration (§1, now also proven for partial
> writes), `glGetBufferSubData` being unsound (§2), the negative
> `FD_MESA_DEBUG` matrix (§5), and the UBWC finding (§5).
>
> The real ISA size is known: `IR3_SHADER_DEBUG=disasm` reports
> `FRAG prog 1/1: 5 instr … 32 dwords` — **5 instructions, 128 bytes** — inside
> a 4096-byte BO.

# Session findings — the kernel is exonerated, and the "empty shader ISA" was a misidentification

This substantially redirects the project. Two long-standing beliefs are now
refuted, and the evidence points somewhere much more tractable.

## 1. The kernel upload and submit path is PROVEN sound

Reproduced `fd_bo_upload`'s exact syscalls from pure ctypes — no gallium, no
Mesa — using `MSM_BO_WC`, the flag freedreno actually gives `FD_BO_NOMAP`
shader buffers (`rawdrm.py`):

```
GEM_NEW ok handle=1 → GET_OFFSET → mmap → memcpy → munmap → mmap
VERDICT: PATTERN ROUND-TRIPPED — DRM paging path is sound
```

Then closed the loop by **submitting** such a BO (`fullpath.py`), with PM4 in a
second BO, and reading the kernel's own `a6xx_submit()` dump:

```
bo  boflags size   nonzero   PAT hits
0   0x20000  4096   1024      1024/1024
TOTAL PATTERN HITS: 1026
VERDICT: userspace writes ARE visible in submitted BOs
```

A buffer written by ctypes and submitted by ctypes reads back **1024/1024**
intact. Userspace writes land in submitted BOs; the mapping, the fault handler,
the SGT and the submit path all agree. **The entire "kernel aliasing" family is
closed.**

This also refuted the Mesa subagent's own top-ranked mechanism (writecombine
store visibility on the `FD_BO_NOMAP` path) — tested directly rather than
accepted.

## 2. `glGetBufferSubData` is unsound here — retire it as a tool

For a GPU-visible buffer, freedreno satisfies it with a **GPU-side copy** into
staging. Because the GPU cannot write, it returns zeros even when the data is
verifiably present:

```
PRE-DRAW glGetBufferSubData : all zeros, hits=0/16
PRE-DRAW map(READ) direct   : 0 0 0 0x3f800000 …      hits=4/16
PRE-DRAW VERDICT: data IS in the BO pages
```

An intermediate run printed `BO IS ZERO — the upload never landed` and **that
verdict was wrong**, for exactly the reason `pattern.py` was wrong. Use
`glMapBufferRange(GL_MAP_READ_BIT)` for any CPU-side readback.

## 3. THE FINDING: the fragment-shader ISA is never allocated or submitted

`bigshader.py` grows its fragment shader via `N = 120` at line 121. Running it
at N=120, N=600 and N=1500:

```
N=120   bo[0]=4096 bo[1]=20480 bo[2]=4096 bo[3]=4096 bo[4]=4096 bo[5]=4096
N=600   bo[0]=4096 bo[1]=20480 bo[2]=4096 bo[3]=4096 bo[4]=4096 bo[5]=4096
N=1500  bo[0]=4096 bo[1]=20480 bo[2]=4096 bo[3]=4096 bo[4]=4096 bo[5]=4096
```

Across the whole N=1500 capture there is **exactly one submit** (`seq=15`,
`nr_bos=6`, `nr_cmds=1`) and **no buffer larger than 20480 bytes**.

A 1500-term fragment shader needs tens of KB of ISA. No such buffer is ever
allocated, uploaded, or submitted. So is `bo[1]` the ISA? **No** — it is exactly
20480 bytes whether the shader has 120 terms or 1500. **`bo[1]` is a fixed-size
buffer, not the fragment shader ISA.**

That means the finding that anchored a large part of this project — "the shader
ISA buffers are empty" — was measuring a **misidentified buffer**. There is no
empty ISA being submitted; there is **no ISA being submitted at all**.

Corroborating: the BO set is identical for a trivial fragment shader (alias3,
`o = vec4(0,0.5,0.5,1)`) and the 120-term one. Same sizes, same emptiness.

## 4. Why that reframes everything

freedreno compiles a stage's variant lazily, at draw time, in
`ir3_shader_variant()` → `upload_shader_variant()`. **If the draw is never
emitted, the fragment shader is never compiled, no ISA buffer is ever
allocated, and the shader domain correctly has nothing to execute.**

That is a far more tractable bug than "the compiler produced zeros", and it
explains the silence without any fault: there is nothing to fault.

The single submit with `nr_cmds=1` may be *only the `glClear`*. The decisive
test is a clear-only vs clear+draw comparison — identical everything except
`glDrawArrays` — comparing `nr_bos`/`nr_cmds`. If they match, the draw is not
being submitted. (`clearonly.py` is written for this; the run was cut short when
the phone froze.)

## 5. Also settled this session

- **GPU is healthy**, verified the documented way via `a6xx_gpu_cpstate`:
  `SPTPRAC_PWR_CLK_STATUS 0x00000000`, `RPMH_POWER_STATE 0x00000000`,
  `CP_TIMESTAMP delta 676008 clocking 1` (the recipe's healthy reference is
  ~675,000). `runtime_status=suspending` is a red herring and must not be used
  as a health gate.
- **`FD_MESA_DEBUG` matrix is entirely negative**, each run health-checked
  before and after, GPU healthy throughout:
  `noblit`, `noubwc`, `sysmem,noubwc`, `flush,inorder,dclear,ddraw`, `nobin`
  → all `[0,0,0,0]`. Mesa's rendering-path configuration space is exhausted.
- **UBWC static registers are correct.** Web research ranked "CAF 4.14 lacks
  mainline's `a6xx_set_ubwc_config()` + `UCHE_GMEM_RANGE`" as hypothesis #1.
  Image 237 already writes exactly what mainline derives:
  `RB_NC_MODE_CNTL=0x14`, `TPL1/SP_NC_MODE_CNTL=0x04`, `UCHE_MODE_CNTL=0x400000`,
  `UCHE_GMEM_RANGE=0x100000+gmem-1`. **Refuted.**
- **Research found no known a630/a640 "renders nothing" erratum at all** — an
  explicit negative, and `a6xx_adjust_ubwc_bank_alignment` does not exist
  upstream (don't chase that name).
- **BO names are unreachable.** `fd_bo_new`/`fd_bo_set_name` wrap their naming
  in `#ifndef NDEBUG`, so release Mesa never issues `MSM_INFO_SET_NAME`. The
  kernel support added in image 258 is correct but dead with the Debian Mesa.
  The version handshake is fine (`major 1, minor 4` = `FD_VERSION_SOFTPIN`).

## 6. Process notes

Two instruments were wrong in a row again (`glGetBufferSubData`; and my own
`BO IS ZERO` verdict built on it). The rule that saved it is that every
harness now **proves its buffer reached `bos[]`** before comparing content, and
reads back through a direct CPU mapping rather than any GL readback.

I also wedged the GPU once by running an `FD_MESA_DEBUG` matrix with no health
check between runs, and once more while debugging — both needed a manual
reboot. Health must be checked via `cpstate` before and after every GPU action.

## Next step

Run `clearonly.py clear` and `clearonly.py draw` on a fresh boot (GPU brought up
with the `a6xx_gpu_load` chain) and compare `nr_bos`/`nr_cmds` in the BOS dump:

- identical → **the draw is never submitted**; chase why Mesa/the driver drops it
- draw has more → the ISA is allocated but dropped; look at the draw path

Either way this is the first evidence that would point at the draw path rather
than the shader compiler.