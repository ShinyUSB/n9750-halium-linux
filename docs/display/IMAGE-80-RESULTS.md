# Image 80 — PDC routing fixed at source; diagnostic boot cannot reach rpmh_init

## Routing fix (applied, build-clean, verified at source)

| register | before | after | authority |
|---|---|---|---|
| `SEQ_MEM_0..4` | `pdcseq_write(seq, 0xa0000+i)` → seq+0x280000 | `pdcseq_write(seq, 0x0+i)` → seq+0x0 | KGSL `PDC_GPU_SEQ_MEM_0=0x0`, v5.8 `pdc_write(seqptr, SEQ_MEM_0)` |
| `SEQ_START_ADDR` | `pdcseq_write(seq, 0x21148)` → seq+0x84520 | `pdc_write(pdc, 0x21148-0x20000)` → cfg+0x4560 | v5.8 `pdc_write(pdcptr, SEQ_START_ADDR)` + KGSL `_regwrite(cfg, SEQ_START_ADDR)` |
| `ENABLE_PDC` | `pdc_write(pdc, 0x21140)` → cfg+0x84500 | `pdc_write(pdc, 0x21140-0x20000)` → cfg+0x4500 | KGSL `0x1140` |
| `TCS1_*` (12) | `pdc_write(pdc, 0x2157x)` → cfg+0x855cc | `-0x20000` → cfg+0x55cc | KGSL `0x157x` (ours = KGSL+0x20000, all 12) |
| `TCS3_*` (12) | `pdc_write(pdc, 0x215dx)` | `-0x20000` | KGSL `0x15dx` (all 12) |
| `TCS*_ADDR+8` value | `0x30080` | `0x30090` | v5.8 `pdc_address_offset` (A640) + KGSL A640 branch |

Image 80: `31636b13b67a19883a9f6cbe4422bcad7ff7f0343cac161c6b5853a8680000a8`.
SDE 0, DT unchanged, IOMMU `fac06af5…`, parser `51f5445199…`, production empty.

## Diagnostic boot result: rpmh_init is unreachable

Four triggers on image 80, all with unbuffered host capture
(`grep --line-buffered /dev/kmsg`, no `cat` pipe):

| run | last line captured | device |
|---|---|---|
| 1 (full stream) | `loaded qcom/a630_sqe.fw` then end | rebooted (uptime 35) |
| 2 (filtered) | `PROBE COMPLETE`, no fw lines | rebooted (uptime 62) |
| 3 (unbuffered) | `clock[1] 'cxo_clk' prepare enter` | rebooted (uptime 58) |
| 4 (unbuffered) | `PROBE COMPLETE`, no fw lines | rebooted (uptime 38) |

The failure point **moves between runs**: post-probe twice, `cxo_clk prepare`
once, `sqe loaded` once. Death is fast (device gone within ~12 s of trigger).
No Oops, no panic, no EXT4 errors in any run.

## Interpretation

A moving failure point with fast silent death, upstream of the fixed code, is
not consistent with a second register-routing bug (which would fault
deterministically at the same instruction, as image 78 did). It is consistent
with the missing power vote: without the CX GDSC / ACD that KGSL's
`gmu_start()` takes before `gmu_enable_clks()`, the clock/power domain state at
each step is undefined, so the wedge lands wherever the hardware happens to be.

The PDC routing fix itself is source-verified against two independent
implementations (v5.8 split + KGSL per-region constants + physical fit within
both 64KB v2 windows) but **remains runtime-unvalidated** — no boot has yet
executed a single corrected PDC write.

## Recommendation

The next change must be the power vote: `regulator_enable(vddcx)` for
`gpu_cx_gdsc` (mirroring KGSL `gmu_enable_gdsc()`) before the clocks, inside
`a6xx_gmu_resume()`. Optionally ACD via the AOP mailbox. Without it, further
diagnostic boots will keep tripping over the same undefined power state before
reaching the code being tested.