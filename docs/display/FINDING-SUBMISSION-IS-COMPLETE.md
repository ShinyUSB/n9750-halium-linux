# FINDING: the submission is complete and correct — the command stream is not the bug

Date: 2026-11-16. Supersedes the "shaders never submitted" and "dangling IOVA"
claims. Image 260, harness `harnesses/flushpath_probe.py` + `harnesses/ndcap.sh`.

## Method

Same clear+draw workload, `FD640`-gated, GBM window surface, true `glFinish`,
no `glReadPixels`. Only `NDRAW` differs (1 vs 11). The kernel's own `ibdump`
dump was captured to a file and parsed byte-for-byte; buffers were joined on
`mmoff`, not `bo[N]`, because the `bo[]` index order is not stable across runs.

**The capture is verifiably complete.** `ndcap.sh` asserts
`headers_captured == nr_bos` and refuses to report a run that fails it. This
matters: an earlier `dmesg -w` capture silently lost 3 of 11 BO headers and
nearly produced another false "dangling IOVA" verdict. Use `dmesg -W`
(`--follow-new`), not `-w`: `-w` prints the whole existing ring backlog first,
and while it does that ~24k new lines overwrite entries it has not read.

## Results

### 1. NDRAW scales the command stream exactly as expected

| | nonzero dwords | `CP_DRAW` (`0x70388003`) packets | last dword |
|---|---|---|---|
| NDRAW=1 | 7 | 1 | 9 |
| NDRAW=11 | 103 | **11** | 112 |

Each draw is a clean 4-dword `CP_DRAW` packet (`0x70388003`, `0x00000184`,
`0x00000001`, `0x00000003`), and 11 draws produce 11 of them. The earlier
"~2 dwords per draw" reading was a measurement artifact — it counted a
different buffer, not the command stream.

### 2. State IS emitted before the draws

The NDRAW=11 stream at `mmoff=0x10217c000` (bo[7], the 4096-byte cmdstream)
begins with a full prologue, not a bare draw:

```
[000] 0x40a00e01   [004] 0x40980301   [006] 0x48888701
[008] 0x48801086   [012] 0xc2000000   [015] 0x4880d002
[018] 0x40800601   [019] 0x0007fdff
[069..] 11x CP_DRAW packets
```

This directly refutes the "~2 dwords/draw = bare CP_DRAW with no state"
reading. Shader loads, viewport, blend and primitive-restrict state are all
present.

### 3. Every relocation target resolves inside a submitted BO

13 aperture-range 64-bit values in the cmdstream, checked for **containment**
(`iova <= target < iova + size`) against the submit's IOVA set:

```
dw 22 -> 0x700019700  INSIDE 0x10216c000 (+0x2700)
dw 25 -> 0x700018700  INSIDE 0x10216c000 (+0x1700)
dw 28 -> 0x700017700  INSIDE 0x10216c000 (+0x700)
dw 31 -> 0x7000198c0  INSIDE 0x10216c000 (+0x28c0)
dw 34 -> 0x70002e000  INSIDE 0x10217e000 (+0x0)
dw 37 -> 0x70002e030  INSIDE 0x10217e000 (+0x30)
dw 40 -> 0x70002e050  INSIDE 0x10217e000 (+0x50)
dw 43 -> 0x7000176c0  INSIDE 0x10216c000 (+0x6c0)
dw 52 -> 0x700019940  INSIDE 0x10216c000 (+0x2940)
dw 55 -> 0x7000175c0  INSIDE 0x10216c000 (+0x5c0)
dw 58 -> 0x7000199c0  INSIDE 0x10216c000 (+0x29c0)
dw 61 -> 0x70002e070  INSIDE 0x10217e000 (+0x70)
dw 64 -> 0x70002e080  INSIDE 0x10217e000 (+0x80)
=> inside=13 outside=0
```

**The image-260 `DANGLING` scan is a false-positive generator.** It tests
*exact base-IOVA equality*, so any relocation pointing into the middle of a
buffer is reported as dangling. Every one of its 7 hits this run is a valid
offset into a submitted BO. That is also what made the earlier
`ROOT-CAUSE-DANGLING-IOVA.md` claim wrong the first time; the same mistake
nearly recurred.

### 4. The referenced shader state is present and non-trivial

At each referenced offset inside `mmoff=0x10216c000` (the 32768-byte state
buffer) the dumped bytes are real a6xx register writes, not zeros:

```
+0x700:  0x40a99e01 0x00007fc0 0x40a9a801 0x40ab0001 0x00000005
         0x40a98c01 0xfcfcfc00 0x40a80001 0x00100100 0x40a82401
+0x2700: 0x40bb0801 0x000000ff 0x48b80004 0x40bb1001 0x48a82301
+0x2940: 0x40800001 0x00000080 0x40809001 0x48809102 0x409b0001
+0x29c0: 0x40882001 0x000007e0 0x40a98901 0x48886501 0xffff0000
```

The `0x40xxxxxx` / `0x48xxxxxx` words are `CP_REG_WRITE` packets (reg in
bits [12:8], `0x1` = write). These are genuine shader descriptor state.

## What this rules out

- **"Shaders are compiled and uploaded but never submitted."** They are
  submitted, and their state is referenced by resolved relocations.
- **"The ISA is empty / all zeros in the submission."** The referenced
  regions contain full register state.
- **"Draws emit a bare CP_DRAW with no state."** The prologue is complete.
- **"Dangling IOVA."** 13/13 resolve.
- **"Mesa never submits."** Retracted separately — that was a deferred submit
  (`freedreno_ringbuffer_sp.c:313`), not an absent one.

## Where the bug therefore is NOT

Not in command-stream construction, not in relocation, not in buffer
bookkeeping, not in BO identity. The userspace→ring path is sound end to end.

## What this points at

The submission is *correct* and still renders black. So the defect is in what
happens **after** the ring is written, i.e. in the handoff to hardware:

1. **Ring handoff / CP kickoff.** Is the ring actually programmed into the
   channel and the CP told to start? The submit returns and fences retire, but
   that has been true of every run.
2. **GMEM vs sysmem render path.** Which path does this tree actually take for
   a 64x64 RGBA8 renderbuffer? `freedreno_gmem.c` picks between
   `render_gmem` and `render_sysmem`; the 4096-byte cmdstream and the
   32768-byte state buffer sizes are consistent with GMEM binning.
3. **The render target BO.** Two 4096-byte BOs in the submit are
   **completely zero** (`mmoff=0x10216b000`, `0x102176000`, `0x102177000`).
   If one of those is the render target, a UBWC-compressed target that is
   never written by the RB is a strong candidate — but it must be identified
   by name/size rather than guessed.

Highest-value next step: confirm from the post-submit path whether the CP
actually consumed the ring, and identify the zero-filled 4096-byte BOs by
role. Both are answerable without a kernel build, by capturing
`/sys/kernel/debug/dri/0/rd` (or the ring wptr/retired values) around a
submit.

## Instrument note

This finding required fixing the instrument, not the hypothesis. Two separate
capture bugs (the `dmesg -w` backlog race; the 8192-dword dump cap silently
skipping large buffers) each produced confident, wrong conclusions before being
caught. The `headers_captured == nr_bos` self-check now runs on every capture.