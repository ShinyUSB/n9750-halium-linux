# Evidence integrity note — x267 captures

Date: 2026-10-18

## What was lost

The two register captures from image 267 are **permanently gone**:

| file | status |
|---|---|
| `/root/x267.txt` (W=64) | **0 bytes** |
| `/root/x267b.txt` (W=128) | **does not exist** |
| `/root/sdiff.txt` (stream-vs-HW diff, never ran) | does not exist |

`/root` is on the persistent rootfs (`/lib/firmware` survived reboots), so this
is not a reboot artefact: `x267.txt` was truncated to zero at 20:45 and
`x267b.txt` was never flushed. This matches the known failure mode recorded in
`STATE-END-OF-SESSION-2026-11-16.md`: a file-based `dmesg -W` capture does not
survive a hard freeze, because the writer process dies with the machine.

Verified on-device: `grep -l X267 /root/*.txt` returns nothing.

## What survives

1. **This repository**, specifically
   `FINDING-MESA-STATE-IS-LIVE-IN-SILICON.md`, which transcribes the observed
   values verbatim from the console output of those runs.
2. All command-stream captures, which are intact locally:
   - `m-sysmem.txt` (460 KB) — sysmem clear stream
   - `nd-e11.txt` (1.5 MB), `nd-d11.txt`, `nd-d1.txt` — GMEM draw streams
   - `nd-r32/64/128.txt` — clear-only streams
   - `trc-sysmem.txt` — **0 bytes** (already lost previously)
3. Compute-probe captures on device, hashed:
   - `/root/wrt2.txt` `a52c8fbab436cfd48def1139223f8147160d08527d2bc007afa04c9e7f6eb1f3`
   - `/root/wrt3.txt` `4e4f2cc2caea14e506da7f50844b51f01b6f10793eb6d44c1bfd4e221734d3c7`
   - `/root/trc-sysmem2.txt` `e546a343c0dcd57c7dd4155a84dc9800603bed85f63bcd813995a7cffbbdec7c`

## Epistemic status of the x267 result — stated precisely

Because the raw captures are lost, the following are **transcriptions, not
re-verifiable artefacts**. They should be treated with that weight:

| Claim | Status |
|---|---|
| `apread` parameter existed and was functional | established (console output, and `X267` strings present in the built `Image`) |
| Aperture values written followed the KGSL formula (`0x0/0x11`, `0x300/0x311`, `0x500/0x511`) | transcribed from console |
| Aperture read back 0 before and after the dump | transcribed from console |
| At W=64, `GRAS_2D_DST_BR=0x003f003f`, `RB_MRT_BUF_INFO=0x2030`, `PITCH=4`, `ARRAY_PITCH=0x100`, `BASE=0x7_00026000` | transcribed from console |
| At W=128, `GRAS_SC_VIEWPORT_SCISSOR_BR_0` = `0x0000007f` (was `0x003f003f`) | transcribed from console — **the load-bearing observation** |
| FE(0), GRAS(3), PS(5) returned identical values | transcribed from console |
| `0x8e3b` reads `0xdeafbead` | transcribed; offset independently corroborated by `KGSL a6xx_reg.h:677` |
| Device froze when the aperture/sub-block sequence was run again on a live GPU | **observed directly**, device required manual reboot |

The load-bearing claim — that register content **tracks the workload** — rests on
a single transcribed before/after pair. It was not captured to durable storage
before the machine froze. That is a real limitation and it should be re-derived
before any irreversible action is taken on the strength of it.

## Standing constraint

Per the 2026-10-18 decision: **no further live register probing.** The
aperture/sub-block write sequence is not to be reproduced. Image 268 is not to
be implemented in its currently proposed form. Investigation proceeds through
source analysis, existing captures, logs, and device-tree data.