# GBIF experiment — prepared, verified, awaiting approval. NOT FLASHED.

Date: 2026-10-18

## 1. The critical discovery that changes this experiment

While preparing it, I found that **`a6xx.uche_gbif_rw` was a complete no-op.**

```
a6xx_gmu.c:1991   bool uche_gbif_rw;                 <- definition
a6xx_gmu.c:3508   module_param(uche_gbif_rw, bool, 0444);
a6xx_gpu.c:2322   extern bool uche_gbif_rw;         <- declaration
a6xx_gpu.c:2399   ...flip with a6xx.uche_gbif_rw=1...  <- a COMMENT
```

There is **no `if (uche_gbif_rw)` anywhere in the tree**, and
`REG_A6XX_UCHE_GBIF_GX_CONFIG` (0x0e3a) is **never written by anything**. IMAGE
233 deferred the write and left behind only the parameter and a comment.

So the experiment as originally conceived would have flashed a flag that does
nothing, produced no change, and **wrongly eliminated candidate B**. That is
exactly the failure mode this project keeps hitting, caught here before it cost a
flash.

Reference for the intended behaviour:
```
KGSL adreno_a6xx.c:804-805
    if (ADRENO_QUIRK(adreno_dev, ADRENO_QUIRK_LIMIT_UCHE_GBIF_RW))
        kgsl_regwrite(device, A6XX_UCHE_GBIF_GX_CONFIG, 0x10200F9);
```
(`a6xx_reg.h:753` = 0xE3A; `adreno.h:159` = BIT(8); quirk table `adreno.c:773`.)

## 2. Code change (image 268)

File: `drivers/gpu/drm/msm/adreno/a6xx_gpu.c`, +38 lines, one hunk.
Patch: `patches/268-uche-gbif-gx-config.patch`

Placed inside the **existing UCHE configuration block**, immediately after the
A640 GBIF interconnect start (`HWINIT[11]`) and alongside the UCHE registers the
driver already programs (`UCHE_WRITE_RANGE_MAX`, `UCHE_TRAP_BASE`,
`UCHE_WRITE_THRU_BASE`, `UCHE_GMEM_RANGE`, `UCHE_FILTER_CNTL`, `UCHE_CACHE_WAYS`) —
i.e. after the GPU is voted and the interconnect is up, not in the pre-GMU-vote
region where the old comment sat.

```c
if (uche_gbif_rw) {
        gpu_write(gpu, REG_A6XX_UCHE_GBIF_GX_CONFIG, 0x10200F9);
        pr_info("HWINIT[27] UCHE_GBIF_GX_CONFIG=0x10200f9 WRITTEN ...\n");
} else {
        pr_info("HWINIT[27] UCHE_GBIF_GX_CONFIG SKIPPED ...\n");
}
```

**WRITE ONLY, NO READBACK.** Images 231/232 established that GBIF-side registers on
this setup do not answer reads — a read never returns and wedges the AXI bus,
hard-freezing the machine. This register is on the GBIF path, so it is treated as
read-unsafe. This also satisfies "do not add register-dumping instrumentation".

Both branches are present in the built binary (verified with `strings`).

## 3. Command line — single-variable change

**`swiotlb=2048` preserved.** No DMA-related setting was removed.

`androidboot.memcg=1` and `a6xx.uche_gbif_rw=1` are both exactly 19 characters, so
this is a **1:1 in-place substitution** — total cmdline length unchanged at 512,
byte-for-byte, with no other token touched.

Justification that `androidboot.memcg` is dispensable here:
- `grep -rn "memcg"` over `drivers/ mm/ kernel/ init/` finds **no consumer** that
  parses it as a boot parameter; the only hits are slab memcg internals that
  obtain their configuration by other means
- It is an `androidboot.*` token, consumed by Android userspace init. This is a
  Debian rootfs with our own `/init`.

Residual risk (stated, not hidden): if something in the rootfs reads
`/proc/cmdline` for `androidboot.memcg`, removing it changes behaviour. Nothing in
the kernel or the build path does. We can restore it in a follow-up image if a
symptom appears.

```
before: ... androidboot.console=ttyMSM0 androidboot.memcg=1 lpm_levels...
after : ... androidboot.console=ttyMSM0 a6xx.uche_gbif_rw=1 lpm_levels...
```

## 4. Artifact integrity verification (performed on the packed image)

The packer's own interlock fired ("NOT a single-variable change") because it
compares against image 242 and assumes only the `id` field moves. That assumption is
now intentionally violated, so I verified the cmdline **by extracting it from the
finished image**, not from the input file.

Extracted from `boot_img_hdr` offset 64, length 512:

| check | result |
|---|---|
| extracted cmdline == `cmdline268.txt` | **True** |
| length | 512 bytes (unchanged) |
| differing bytes vs reference 242 cmdline field | offsets 166–181 only (**16 bytes**) |
| 242 at that region | `ndroidboot.memcg` |
| 268 at that region | `6xx.uche_gbif_rw` |
| 268 cmdline == 242 cmdline with **only** that swap | **True** |
| `a6xx.uche_gbif_rw=1` complete, not truncated | **True**, index+19 ≤ 512 |
| `swiotlb=2048` preserved | **True** |
| `_probe=1`, `a6xx.experimental_nonlegacy=1`, `a6xx.a6xx_selftest=1` | all present |
| `a6xx.gpu_init_delay_ms=300000`, `androidboot.console=ttyMSM0` | present |
| `firmware_class.path=/vendor/firmware_mnt/image` | present |
| `ramoops.mem_address=0xc1300000`, `a6xx.force_gx_clk=0` | present |
| ramdisk sha vs pinned `eb5d2eb5…` | verified by packer |
| `HWINIT[27]` instrument in packed kernel payload | **True** |

### Hashes
```
268 image     5e79a9f91c04232818b19f4f5bd75bcb683e1bfa560180d072dc830fe605d3ac
              /home/usb/artifacts/flash/268-halium-boot-a6xx-uche-gbif.img
kernel        4fc7cdbbeba1f74df936534d6e87f70d614f21325bd871f7fefd10e1ae717ec8
cmdline       4c8d860dea163db1c108a65466d7a9d50435ca13e43bb526e5f10ccaba7b0d03
```

The image to be flashed is exactly the artifact inspected above; the hash is
recorded so that can be re-checked immediately before `heimdall flash --BOOT`.

## 5. Falsifiability — how we know the parameter was *consumed*

The user's requirement, and the reason this experiment is meaningful:

| level | evidence | what it rules out |
|---|---|---|
| 1 | `strings` on the packed kernel shows the `HWINIT[27]` WRITTEN branch | patch not in the image |
| 2 | `/proc/cmdline` contains the **complete** token `a6xx.uche_gbif_rw=1` | cmdline truncation (the original failure) |
| 3 | `HWINIT[27] ... WRITTEN (a6xx.uche_gbif_rw=1)` in dmesg | token did not reach the driver |
| 4 | workload result | the actual hypothesis |

**Level 3 is the decisive one** and exists only because I added the `pr_info` to
both branches. If `HWINIT[27]` says SKIPPED, the token was truncated or the param
did not bind, and **the experiment is void** — we do not conclude anything about
GBIF.

**A negative result only downgrades the hypothesis if levels 1–3 pass AND the
workload exercised the relevant path.** Otherwise it is uninterpreted.

Evidence capture, per the requirement not to rely on filesystem writes that a hard
freeze would destroy: use `dmesg -W` **and** serial (`androidboot.console=ttyMSM0`)
where available; `ramoops` is configured (`ramoops.mem_address=0xc1300000`,
`console_size=0x40000`) and survives a hard reset. Capture dmesg to file *and*
record the `HWINIT[27]` line text in the report, so the conclusion does not depend
on a file that can be truncated.

## 6. Workload — unchanged from the established baseline

No new instrumentation, no register dumping, no aperture switching.

- `wrt2.py` T1 (compute → SSBO) and T2 (transform feedback → TBO), both
  oracle-validated by a CPU `0xAA`/`0x5A` prefill round-trip
- `pixelreadback.py` (6 checks) for the render path
- Baseline for comparison is image 267 with the newer SQE ucode:
  T1 `256/256` prefill intact, `0/64` pattern matches;
  T2 `1024/1024` intact, `0/64`; render `0/6`.

## 7. Recovery

- Known-good rollback images present: `263-halium-boot-a6xx-rb.img`,
  `242-rollback.img`, and `267-halium-boot-a6xx-aperture.img`
- Recovery = `heimdall flash --BOOT <rollback>`; BOOT-only, no PIT, no repartition,
  no userdata, no erase
- `/lib/firmware/qcom/a630_sqe.fw.orig` also retained if the ucode must be reverted
- Risk class: one vendor-proven register write already present in our tree behind a
  parameter. Not an experimental PM4 packet, not a live-hardware probe. It does
  carry the (small) risk that writing an untested GBIF-side register on this silicon
  misbehaves — hence write-only, no readback, and a clean rollback.

## 8. Status

**Awaiting approval. Not flashed.** All integrity checks pass; the decision to
flash is yours.

## 9. Expected outcome and what follows

- **Compute and/or render writes appear** ⇒ B supported. Next task: determine the
  correct gating (the `ADRENO_QUIRK_LIMIT_UCHE_GBIF_RW` DT quirk value) rather
  than leaving an unconditional write, then re-verify the full matrix.
- **Still dead, with HWINIT[27] = WRITTEN** ⇒ B eliminated. With A (flush present
  and ordered) and C (buffers mapped read-write) already eliminated, the remaining
  space collapses to GBIF transaction formation below `UCHE_GBIF_GX_CONFIG`, or to
  something in the interconnect/QoS programming at `HWINIT[11]`.
- **HWINIT[27] = SKIPPED** ⇒ experiment void, no conclusion. Fix the cmdline and
  retry.