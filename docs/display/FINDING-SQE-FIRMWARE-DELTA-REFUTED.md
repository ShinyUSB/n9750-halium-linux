# FINDING: SQE firmware delta REFUTED — newer a630_sqe.fw changes nothing observable

Date: 2026-11-16
Baseline: image 263 (unchanged — **no rebuild, no flash**; this experiment was a
file copy plus a reboot)

## What was changed

Swapped the SQE ucode blob for the current linux-firmware master version.
`adreno_device.c:110` assigns `[ADRENO_FW_SQE] = "a630_sqe.fw"`, so this is the
blob the driver actually loads.

| | size | md5 | dword[1] (version) |
|---|---|---|---|
| before | 32304 | `42b0a1566c41917df56f15f7d9c7c479` | `0x016ee190` |
| after | 34188 | `9f2540d789d9fd4699a566d97fcabded` | `0x016ee207` |

Source: `git.kernel.org/.../linux-firmware.git/plain/qcom/a630_sqe.fw`
(local copy: `/home/usb/artifacts/fw/a630_sqe.fw.linuxfirmware-master`,
sha256 `1c21b527d9183487cc550dabbb3f43e555df5a977a461934fc61f0635a9aa90c`).
Backup retained on device at `/lib/firmware/qcom/a630_sqe.fw.orig` (verified
`42b0a156…` after install).

Upstream ships **no** `a640_sqe.fw`; `a630_sqe.fw` is the correct blob for A640,
so this is a like-for-like upgrade, not a substitution.

## Bring-up after reboot

```
SELFTEST142: done last_iter=10 ALL 10 PASS
HWINIT[25] return ret=0
SQE213: name=a630_sqe.fw size=34188 b0=0x00 b2=0x00 ver=0x000 nib0=0x0 nib2=0x0 would_whereami=0
CP_TIMESTAMP … clocking 1
```

The new ucode loads and initialises cleanly. `SQE213: size=34188` independently
confirms the **new** blob is the one resident.

## Measurements after the swap

| Test | Oracle | Prefill intact | Expected | Verdict |
|---|---|---|---|---|
| T1 compute → SSBO (`NDISP=64`) | pass | **256/256 = 0xAA** | 0/64 | **GPU wrote nothing** |
| T2 vertex → transform feedback | pass | **1024/1024 = 0x5A** | 0/64 | **GPU wrote nothing** |
| render `pixelreadback.py` | FBO status COMPLETE | — | **0/6 checks passed** | **all zeros** |

`pixelreadback.py` results in full — plain clear, second colour on the same FBO,
scissored clear inside *and* outside the rect, and a shader triangle at two
positions: every single one returned `(0,0,0,0)`.

## Verdict

**REFUTED.** A 23-year-newer SQE microcode — one that upstream ships and that
passes its own version gate — produces byte-identical dead behaviour. The stale
microcode hypothesis is dead.

This closes the one hypothesis that was simultaneously (a) new, (b) untested, and
(c) free. It had been the top of the shortlist because the old blob's version
nibbles (`0x190`) sit below what upstream's `a6xx_ucode_check_version()` demands,
and our tree never calls that check — so it was a genuinely plausible "wrong
microcode silently running" story. It is now disproven by measurement.

## Consequences for the search

Combined with `FINDING-COMPUTE-WRITE-PATH-ALSO-DEAD.md`, three of the four
"shared write path" candidates are now dead:

- ~~stale/wrong SQE microcode~~ **REFUTED here**
- ~~RB-specific mechanisms~~ **excluded by compute also being dead**
- ~~Mesa render state~~ **excluded by field-level verification**

What survives is the genuinely shared, genuinely unmeasured hardware path:

- UCHE / write-back cache flush or invalidation on the store path
- GPU→DDR transaction formation / GBIF
- SMMU behaviour on the **write** direction (the fence write proves translation,
  but the CP's store may take a different path than a shader store)
- a secure / privilege gate on the shader write path

All four are *below* anything the command stream can express, and all four are
readable through the **aperture-correct** register route — which is exactly the
X1 experiment, now promoted from "did the state land?" to "what does the shared
write-path state say?".

## Note on the blob

`SQE213` reports `b0=0x00 … would_whereami=0`, so this blob would also fail
upstream's version gate even though it is the current upstream artifact. That
confirms the gate in that code comment does not describe this blob's header
layout, rather than indicating a bad file. Irrelevant to our tree, which never
calls the check.

## Revert

```
cp /lib/firmware/qcom/a630_sqe.fw.orig /lib/firmware/qcom/a630_sqe.fw
reboot: echo b > /proc/sysrq-trigger
```
No reflash needed on any branch. The swap is measured-inert, so keeping the newer
blob is defensible; reverting costs one reboot plus the ~305 s bring-up window.