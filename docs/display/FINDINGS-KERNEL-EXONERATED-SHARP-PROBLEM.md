# Where we actually are — a much sharper problem, and what is now ruled out

## The single most important measurement of the session

`IR3_SHADER_DEBUG=disasm,nocache` finally worked, and Mesa reports exactly
what it compiled and uploaded:

```
; FRAG prog 1/1: 5 instr, 0 nops, 5 non-nops, 4 mov, 0 cov, 32 dwords
; VERT prog 2/1: 1 instr, 0 nops, 1 non-nops, 0 mov, 0 cov, 32 dwords
; BVERT prog 2/2: 1 instr, ...
```

So the fragment shader ISA is **5 instructions = 32 dwords = 128 bytes**.
`fd_bo_cache_alloc` rounds that to the 4096 bucket, so the ISA BO is 4096 bytes
of which only the **first 128 bytes** are ever written.

The kernel's BOS dump shows that BO as **0/1024 nonzero** — the head, where the
5 instructions must be, is zero too.

## The kernel is now exonerated, including for partial writes

Three separate proofs, all using raw ctypes with no gallium and no Mesa:

| Test | Write | Kernel sees |
|---|---|---|
| `fullpath.py` | 4096 B into a 4096 B WC BO | **1024/1024** |
| `partial.py` HALF | 2048 B into 4096 B | **512/512** |
| `partial.py` SHORT | **128 B into 4096 B** | **32/32** |
| `partial.py` QUARTER | 1024 B into 4096 B | **256/256** |

The 128-byte case is the exact shape of the shader upload, and it lands in full.
**Short memcpys into a writecombine mapping are not lost.** So "partial writes"
is ruled out, and with it the entire kernel-side family.

## Correcting two claims of mine from earlier in the session

1. **"The draw is never emitted."** Wrong, and I should not have said it. The
   A/B was: clear-only gives a command stream with 240 nonzero dwords,
   clear+draw gives 244. The draw *is* emitted; I misread the identical BO
   sets as "nothing happened".
2. **`bo[1] = 20480` is not the fragment shader ISA.** Growing the shader from
   N=120 to 600 to 1500 leaves every BO size unchanged, and a **clear-only run
   with no draw at all** still contains a 20480-byte BO. It is a fixed-size
   buffer with a different purpose. Much of this project's "empty shader ISA"
   reasoning was about this misidentified object.

## So what remains is narrow

Using the flags to separate FD_BO_NOMAP (shader/pvtmem/tess — `FD_BO_NOMAP`
means no `CACHED_COHERENT`, so `MSM_BO_WC` alone, i.e. `boflags=0x20000`) from
the ring command streams (`FD_BO_GPUREADONLY`, i.e. `0x20002`):

```
bo[0] boflags=0x20000  4096    1 nonzero     <- NOMAP class
bo[1] boflags=0x20000  20480    0 nonzero    <- NOMAP class, fixed size
bo[2] boflags=0x20000  4096     0 nonzero    <- NOMAP class  <-- FS ISA?
bo[4] boflags=0x20000  4096     1 nonzero    <- NOMAP class
bo[3] boflags=0x20002  4096   72/149/244     <- ring command streams
```

`bo[2]` — 4096 bytes, `FD_BO_NOMAP`, zero nonzero dwords — is the best candidate
for the FS ISA BO, and it is empty. Yet the partial-write test proves that
writing 128 bytes into exactly such a buffer and submitting it works.

So the contradiction is sharp: **Mesa compiles 5 instructions, uploads 128
bytes into a 4096-byte NOMAP/WC BO, the draw is emitted, and the kernel reads
zeros — yet the identical operation done from ctypes lands perfectly.**

The difference between the two cases is now the entire search space:
freedreno's BO cache / object identity (the ctypes path uses a fresh BO and
never recycles one), versus page state between upload and submit.

## The one experiment that would settle it

Map the draw's shader reference to a BO index, authoritatively. Attempts to
decode `CP_LOAD_STATE6` (opcode 0x34 FRAG / 0x36 VS) from the dumped stream
failed twice on header layout — I guessed the bit fields both times and both
guesses were wrong, which is exactly the trap that produced tonight's earlier
false claims. I stopped rather than guess a third time.

Reliable options, none of which require guessing packet formats:

1. **Kernel-side**, and unambiguous: in `a6xx_submit()`, log each `submit->bos[b]`
   entry as `obj pointer + size + boflags + iova` (already done in image 258),
   **and** log which BO the ISA would be by printing the same fields for every
   BO that Mesa references. Better: implement `MSM_INFO_SET_NAME` *and* make
   freedreno call it by lifting the `NDEBUG` gate — but we cannot rebuild Mesa.
2. **Userspace-side, no format guessing**: in the Mesa process, after the draw,
   walk `/proc/self/maps` for the `MAP_SHARED` VMAs on the DRM fd, read the
   first 64 bytes of each, and compare against the 32 dwords of ISA that
   `IR3_SHADER_DEBUG=disasm` says were uploaded. If a VMA holds the ISA, the
   write landed in userspace and the loss is kernel-side at submit. If no VMA
   holds it, the memcpy never reached the mapped pages.
3. **Cheapest discriminator first**: run the same GL test twice in one process
   — once normally, once with a `glMapBufferRange(GL_MAP_READ_BIT)` on every
   shader-BO-sized mapping — to see whether the ISA is present in the userspace
   VMA at all. That separates "upload never landed" from "landed then lost".

## Corrections owed to the record

- `ROOT-CAUSE-USERSPACE-WRITES-INVISIBLE.md` — retracted (null test).
- "The shader ISA buffers are empty" — the buffers in question were
  misidentified; the real FS ISA is 128 bytes inside a 4096-byte BO and the
  no-draw control shows the 20480-byte buffer is not a shader at all.
- `ALIAS3-RESULTS.md`'s "BO IS ZERO" verdict — wrong, `glGetBufferSubData` is a
  GPU-side copy and unsound here.
- The subagent's strongest mechanism (writecombine store visibility) is refuted
  by direct measurement; its disk-cache mechanism cannot apply because no ISA
  buffer is allocated at all, and cache hits were excluded anyway.

## Standing hazards

- `glGetBufferSubData` is **not** a sound readback on this device. Use
  `glMapBufferRange(GL_MAP_READ_BIT)`.
- Health must be checked via `a6xx_gpu_cpstate`
  (`SPTPRAC_PWR_CLK_STATUS`, `RPMH_POWER_STATE`, `CP_TIMESTAMP … clocking 1`)
  before and after every GPU action. `runtime_status=suspending` is a red
  herring. Two wedges this session, both needing a manual reboot.
- The GPU needs `echo 1 > …/a6xx_gpu_load` on every boot or Mesa silently falls
  back to llvmpipe, which renders a perfect `glClear`.