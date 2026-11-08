# Image 202 — the firmware load path was write-combine mapped; fixed

Date: 2026-10-11. Kernel `#147`. Built, packaged, **not yet flashed** (needs
physical Download mode).

## The finding

The dominant failure mode — call it **Mode A** — is that the CM3 never executes:

```
fw_start end ret=-110
  FORENSIC POLL ms~5..50 INITRES=0x00000000   (all 10000 samples)
  CM3_CFG=0x4052 BOOTCFG=0x2 WFI=0x0 FWBUSY=0x0  (all correct)
  0 "Unhandled context fault"
  0 GMU watchdog expiries
```

Note what is NOT there: no SMMU fault, no BADDFACE, no watchdog. The mapping is
valid, the permissions are correct, the clocks are on, `CM3_CFG`/`BOOT_CONFIG` are
right. **Only the data is wrong.** That combination had never been explained.

## Root cause

`a6xx_gmu.c` had two allocators. Image 199 converted one to coherent memory and
left the other alone:

| helper | mechanism | buffers |
|---|---|---|
| `a6xx_gmu_memory_alloc()` | `dma_alloc_attrs()` — coherent | `hfi`, `debug` |
| `a6xx_gmu_memory_alloc_fixed()` | `alloc_page()` + `vmap(pgprot_writecombine())` — **not coherent** | `icache`, `dcache`, `itcm_hole`, `log`, `sentinel`, `dummy` |

Image 199's own comment names the bug exactly:

> `pgprot_writecombine` is the bug. A write-combine mapping is NOT coherent with a
> separate bus master … the GMU is its own processor reading through the SMMU, not
> the CPU reading a cache line.

That fix reached the HFI queue and the debug region. **It never reached the buffers
that hold the firmware the CM3 executes from.**

Decoding the real `a640_gmu.bin` block table (sha `3ff0c027…`, verified against the
blob on the phone) shows the split:

```
ITCM   0x00000  14892 B  -> gmu_write_bulk() -> memcpy_toio() + wmb()   ORDERED
DTCM   0x40000   6964 B  -> gmu_write_bulk() -> memcpy_toio() + wmb()   ORDERED
ICACHE 0x04000  14936 B  -> fw_block_mem() -> memcpy() into WC vmap      NOT ORDERED
DCACHE 0x57000    776 B  -> fw_block_mem() -> memcpy() into WC vmap      NOT ORDERED
```

**41.7% of the image — including the single largest block, the CM3's instruction
cache — was written through a write-combine CPU mapping.** The CM3 reads that
memory through the SMMU as an independent bus master. WC stores sit in the CPU's
store buffer and are not observable by any other master until they drain.

There was also **no `wmb()` anywhere** between `a6xx_gmu_fw_load()` returning and
the `CM3_SYSRESET=0` release.

## Why this explains everything, including the parts that seemed contradictory

- **No SMMU faults.** The mapping and permissions are valid. Garbage *data* in a
  valid mapping produces no translation fault. This is the signature that made
  this invisible for 200 images.
- **A lottery, not a failure.** WC drain timing is nondeterministic. Sometimes the
  stores land before the CM3 fetches (~1 ms, perfect boot), usually not.
- **Why images 195/199/200 changed nothing.** All three are *downstream* of the
  firmware load. 195 added a barrier on the HFI transmit path; 199 made the HFI
  queue coherent; 200 added the third TX barrier. If the CM3 never reaches
  `__main`, it never reads an HFI queue. Their failure to help is a **prediction**
  of this hypothesis, not evidence against it.
- **Why image 192's ITCM hole helped but did not finish.** It removed
  *host-visible* SMMU faults (5→0). It could not make the firmware's own fetches
  coherent, because it was never about coherency.

## The change

1. `a6xx_gmu_memory_alloc_fixed()` now uses `dma_alloc_attrs(..., DMA_ATTR_FORCE_CONTIGUOUS)`
   + a single `iommu_map()` at the same caller-supplied fixed IOVA with the same
   flags, matching CAF `kgsl_gmu.c:139` `alloc_and_map()` — which uses
   `dma_alloc_attrs()` for *every* GMU buffer including `GMU_ICACHE`/`GMU_DCACHE`
   (only `GMU_ITCM`/`GMU_DTCM` return early). `bo->phys` is now non-NULL for these,
   so the existing `dma_free_attrs()` branch of `a6xx_gmu_memory_free()` handles
   them; the per-page branch is dead code kept as a safety net.
   - `itcm_hole` keeps its exact IOVA `0x0`, size `SZ_16K` and
     `IOMMU_READ|IOMMU_WRITE|IOMMU_PRIV` — it is not a CAF buffer and image 192
     proved it load-bearing.
2. `wmb()` after `a6xx_gmu_fw_load()` and before the `CM3_SYSRESET=0` release.
   Redundant once the allocation is coherent, but free, and it closes the last
   ordering hole of the same class as images 195 and 200.

`a bare wmb() would NOT have been sufficient`: `alloc_page()` pages also have a
cacheable linear-map alias. `__GFP_ZERO` dirties lines through that alias while
the firmware `memcpy` bypasses cache through the WC alias, so a later eviction can
overwrite the firmware with zeros. `dma_alloc_attrs()` gives one coherent mapping
and does the cache maintenance itself. This is why the allocation change, not the
barrier, is the load-bearing part.

3. **`a6xx_gmu_reset()` now re-asserts `CM3_SYSRESET` before re-running
   `fw_start`.** A separate, independent bug found in the same session.

   `a6xx_gmu_fw_start()` only ever *deasserts* `CM3_SYSRESET`. So a retry re-runs
   the whole programming sequence and then writes 0 to a register that is already
   0 — no reset edge, no CM3 restart. The log proves it: a good attempt 0 reads
   `PRE SYSRESET=0x1`, every retry reads `PRE SYSRESET=0x0`. **The "3 attempts"
   has always been one real attempt per power cycle**, which is why this looked
   like a lottery and why re-flashing to win it was always futile.

   Now: assert, poll `INITRES` to confirm quiescence, then let `fw_start`
   release. Logs `RETRY202: CM3 re-asserted SYSRESET, INITRES now 0x…`.

## Verification before flashing

- Build: exit 0. The 3 `unused variable` warnings in `a6xx_gmu.c` (lines 782, 783,
  2378) are **pre-existing** in untouched functions (`a6xx_gmu_hfi_start`,
  `a6xx_gmu_memory_alloc`) and are present in the 201 tree.
- Disassembly of the built `vmlinux`, `a6xx_gmu_memory_alloc_fixed`:
  ```
  bl  kmem_cache_alloc_trace
  bl  dma_alloc_from_dev_coherent      <- coherent allocation
  bl  iommu_map
  bl  dev_err                          <- map-failure path
  bl  dma_release_from_dev_coherent   <- error path
  bl  kfree
  ```
  **No `vmap`, no `__vmalloc*`** anywhere in the function.
- `a6xx_gmu_fw_start`: `dsb st` (compiled `wmb()`) at `+0x6d0`, immediately after
  the firmware-load `msm_writel` loop, before the sentinel check, before
  `CM3_CFG`/`QTBL`/`SYSRESET`.
- Image: **byte-identical to image 201** in header (all 10 fields), cmdline, board
  name, page size, total size (52543488) and ramdisk (`eb5d2eb5…`, the same
  ramdisk 181-183 used). Only the kernel differs. Verified by unpacking both.

```
artifacts/flash/202-halium-boot-a6xx-coherent.img
sha256 db9ee43851aeae692a9607f2982f2737040db773039ad8bb9c6c837c92695109
kernel sha256 afec5f8588ddacee374fa83e183d2b50231eb8258df339937ef4c544a6f93fa4
```

All three marker strings confirmed present in the final `vmlinux`
(`ALLOC202 iova=`, `RETRY202: CM3 re-asserted`, `NOT physically contiguous`), and
zero `vmap`/`__vmalloc` references remain in `a6xx_gmu_memory_alloc_fixed`.

## Robustness added after adversarial review

A consultant correctly flagged that `dma_alloc_attrs(..., 0)` gives **no
contiguity guarantee**, and a single `iommu_map()` over `dma_addr` is only valid
if the allocation is physically contiguous. This kernel has a **16 MiB CMA**
(`CONFIG_CMA_SIZE_MBYTES=16`), and icache/dcache are 240 KiB each.

Two mitigations, both cheap:

1. `DMA_ATTR_FORCE_CONTIGUOUS` added to both allocators, routing through
   `dma_alloc_from_contiguous()` and making failure explicit rather than silently
   mapping a partial range. Sizes are 4 KiB-240 KiB, well inside the atomic pool.
2. A **runtime contiguity proof** at probe time, logged once per buffer:
   ```
   ALLOC202 iova=0x%llx size=0x%zx phys=0x%llx contiguous=%d
   ```
   walking every page and comparing both the CPU pointer and `virt_to_phys()`
   against arithmetic expectation, plus a `dev_err` if it ever fails. This settles
   the question in the log on every boot rather than by assumption — which is the
   standard this whole bug class failed.

**IOVA overlap re-checked and clean:** `itcm_hole` 0x0-0x4000 ends exactly where
`icache` begins; `icache` 0x4000-0x40000 ends exactly where `dcache` begins;
`dummy` occupies the fixed 0x60000000 and `gmu->uncached_iova_base` is then
*explicitly* set to 0x60001000, so the dynamically allocated HFI queue advances by
1 MiB to 0x60010000 — clear of the fixed `log` at 0x60006000. `sentinel` at
0xDEADB000 is in its own window.


## Decision rule, declared BEFORE flashing

Read `/root/tally.txt` after N boots (the boot-time tally added this session
appends one parsed line per boot, surviving resets):

- **`babeface` count goes from "rare" to present on (nearly) every boot** →
  coherency was the cause. Promote and move to Phase 3 (image 201's HFI poll,
  which is finally reachable).
- **rate unchanged** → coherency is not it. Record as closed and do not re-derive.

Either way the boot-time capture gives us a real rate for the first time, which
this project has never had.

### Secondary read-outs

- `ALLOC202 ... contiguous=1` on every buffer → the contiguity hardening works and
  the single `iommu_map()` is valid. Any `contiguous=0` is a new finding on its own.
- `RETRY202: CM3 re-asserted` appearing, followed by a *second* `BABEFACE` in the
  same boot → the retry fix works independently of the coherency fix. That would
  be the cleanest possible confirmation, since it needs no rate at all: it proves
  a second real boot attempt is now possible.

Two independent hypotheses, two independent confirmations. If both land, Mode A
is solved. If only the retry lands, retries work but attempt 0 still fails often,
and coherency is still open. If neither, both are closed and the next target is
CM3 clock/reset/RPMh gating.

## Also added this session (fixes the measurement, not the driver)

`bootcap.service` + `/usr/local/bin/bootcap.sh`, enabled on the device:

- attaches `dmesg -w` at `multi-user.target`, **before** the GPU load thread and
  far before the time-based bring-up (t=36-117 s), so the "harness attached too
  late and the whole bring-up was invisible" flaw that invalidated the
  `HARNESS-BUG-CAPTURE-TIMING.md` measurement can no longer happen;
- writes to `/root/`, which survives the hard resets;
- appends one parsed verdict line per boot to `/root/tally.txt`:
  `babeface`, `baddface`, `timeouts`, `hfi_ok`, `hfi_to`, `hfi_polls`, `acks`,
  `hwinit_ok`, `suppressed`, `smmu_faults`, `wptr`, `rb_iova`, `clocking`.

Verified working on the live device (syntax-checked, ran, produced a tally line).

## Not changed

Images 187/189/190/191/192/193/195/196/197/198 all untouched and still
load-bearing. No TCM-aperture readback added anywhere (image 183 proved that
aborts the bus and TZ-resets the SoC on the retry path).
