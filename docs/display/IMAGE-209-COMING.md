# Image 209 — measurement only: what the CPU actually wrote to the ring

Date: 2026-11-11. Built, packaged, **not yet flashed**. **Changes no behaviour.**

## Baseline re-established first

Reflashed the existing, hash-verified 205 image rather than a fresh build:

```
artifacts/flash/205-halium-boot-a6xx-norecover.img
sha256 e3c5dbf39feb883cb41e3b4972a9c184a1f04cec0539ac359c955cf4c05c8227
```

Verified on hardware:

| check | result |
|---|---|
| bring-up | `BABEFACE ms~1`, 7/7 HFI ACKs |
| `CPDUMP[auto-rest]` | present |
| `rb_iova` / `RB_CNTL` | `0x1000` / `0x20c` |
| `RB_RPTR_ADDR` | **`0x0`** — reproduces on a fresh boot |
| **`GL_RENDERER`** | **`FD640`** — real driver, not llvmpipe |
| clears ×3 | `err=0x0` |

`RB_RPTR_ADDR = 0x0` is now confirmed on two independent boots, not a one-off.
Since `rptr` is at offset 0 in `struct msm_rbmemptrs`, that is
`memptrs_iova == 0`, so `rbmemptr(ring, fence) == 4` — the `CACHE_FLUSH_TS`
event is told to write the fence at IOVA 0x4.

## What this image adds

Pure `pr_info`, no logic touched. Three groups.

### 1. `RINGCPU` — the ring as the CPU sees it, right after the stream is built

Emitted in `a6xx_submit()` after the last `OUT_RING` and before `a6xx_flush()`,
for `submit->seqno <= 3`:

```
RINGCPU[seq=1] start=<vaddr> cur=<dwords> next=<dwords> size=32768 iova=0x1000
RINGCPU[seq=1] memptrs=<vaddr> memptrs_iova=0x0 fence=<n> rptr=<n>
RINGCPU[seq=1] [nnn] = 0x........      (up to 16 dwords below cur)
```

### 2. `RINGHW[04..06]` — the address spaces side by side

```
RINGHW[04] memptrs=<vaddr> memptrs_iova=0x…  rb_memptrs=<vaddr> rb_memptrs_iova=0x…
RINGHW[05] rbmemptr(rptr)=0x…  rbmemptr(fence)=0x…
RINGHW[06] gpu aspace=<ptr> mmu=<ptr> gmu_domain=<ptr>
```

`memptrs_iova` has never been printed anywhere in this driver, which is why a
zero went unnoticed for 200+ images.

## Why this is the right next measurement

The whole investigation is stuck on one fork, and these lines settle it:

| if the `RINGCPU` dwords… | then |
|---|---|
| are the real packets (`0x7c0000xx` headers, `CP_EVENT_WRITE_0_EVENT`, the fence address as a payload) | the **CPU side of the ring is correct**. The fault is GPU-side translation. That promotes `ROOT-CAUSE-MISSING-SMMU-TABLE-UPDATE.md` from medium to primary, and the next image adds upstream's submit prologue (`CP_SET_PROTECTED_MODE`=0, `PERFCTR_SRAM_INIT_CMD`, `CP_SMMU_TABLE_UPDATE`) plus the `CP_SMMU_TABLE_UPDATE_0..3` field macros that do not yet exist in `adreno_pm4.xml.h`. |
| are zeros, `0xdeadbeef`-like garbage, or stale leftovers | the CPU is **not** writing the memory the GPU reads, and ring coherency is live after all. |

Reading the dwords also validates or kills the `CP_SCRATCH_REG2 == 0` signal —
if the scratch write is plainly present in the stream and the register still
reads 0, the register is either not readable or never executed, and we finally
know which.

`RBBM_INT_0_STATUS` after the submit would be the natural companion, but reading
it needs the live register path and image 205 already showed that a single
spurious hangcheck triggers `a6xx_recover`, which destroyed the instrument on
image 204. Keeping this image inert is deliberate.

## Rules for the run

- **`GL_RENDERER` must read `FD640` first.** Stage results on `llvmpipe` are
  meaningless; that gate has already cost two images.
- Preserve `dmesg` output before the phone reboots — the ring dump is only in
  the ring buffer, and `/tmp` does not survive a reboot.

## Verification

- Build exit 0, no new warnings (the Kconfig `SEC_DEBUG` warnings are
  pre-existing).
- `RINGCPU` strings present in `vmlinux`.
- Byte-identical to 205 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/209-halium-boot-a6xx-ringdump.img
sha256 1084f73a99210f3afd82d3a0d9e064f7d224ae5d430d3bcaf0d3e366c4504086
```

## Not changed

No behavioural change of any kind. Images 187–205 untouched; the 206 and 208
edits remain reverted.
