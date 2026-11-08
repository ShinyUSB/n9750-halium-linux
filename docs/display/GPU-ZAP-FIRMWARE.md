# GPU zap firmware — extracted from `apnhlos`

The Samsung-signed `a640_zap` blobs, resolved. Closes the "ZAP blobs still need
the stock dump — last priority" item in `RESUME.md:134`.

```
/home/usb/mnt-sda/a6xx-build/fw/gpu-fw/
  a640_zap.mdt   6860 B  c9e398...  sha256 de3777f460f0dff729f93caf5a6dba5fb9f77a9fd9174ea4b782e29225876d52
  a640_zap.b00    148 B  sha256 c9e39868fb6edcc30fc3574657f10dd1dfe22c9cd79e53de213a69703582091f
  a640_zap.b01   6712 B  sha256 79e020acb71453feecccafcee7b731f8b8ae02444e14cc8c2420c16d09ea058a
  a640_zap.b02   1968 B  sha256 e24a4f483b71ea19715910f01e1abf94280756df731cad05df48fd7714e34198
```

## Where they actually live: the `apnhlos` FAT filesystem

Not where anyone looked first. This is the non-obvious part, so it is recorded
explicitly:

```
/dev/block/sda16          ->  by-name/apnhlos   (vfat)
  mount -t vfat /dev/block/sda16 /mnt
  /mnt/image/a640_zap.mdt
  /mnt/image/a640_zap.b00
  /mnt/image/a640_zap.b01
  /mnt/image/a640_zap.b02
```

They sit in `/mnt/image/` right next to `a640_gmu.bin` and `a630_sqe.fw` — the
GPU firmware bundle is in `apnhlos`, not in vendor, not in the modem
partitions.

## Places searched first, all negative

Recorded so the next person does not repeat it:

| location | device node | result |
|---|---|---|
| `modemst1` | `sda1` (2 MB) | no hit |
| `modemst2` | `sda2` (2 MB) | no hit |
| `fsc` | `sda3` | no hit |
| `efs` | `sda6` (20 MB) | no hit |
| `fsg` | `sdd3` | no hit |
| `modem` | `sda17` (85 MB) | no hit |
| `hyp` | `sdd31` | no hit |
| `aop` | `sdd5` | no hit |
| `boot.img` | — | name strings only (KGSL gpulist + a DTB `qcom,firmware-name` property), **not blobs** |
| `vendor.img.raw` | — | absent; `/firmware` holds only `a640_gmu.bin`, `a630_sqe.fw` |
| `system.img.ext4`, `dtbo.img`, `persist.img.ext4` | — | absent |
| stock ROM tar (`AP_N9750...user_low_ship_MULTI_CERT_meta_OS12.tar.md5`) | — | **contains no `modemst*` at all** — it is a `user` package, which by design excludes modem firmware |

The two `boot.img` string hits are false positives worth remembering:
offsets `34499210` (KGSL's firmware-name table) and `49983256` (a
`qcom,firmware-name` DT property). Both are names, not payloads.

## Validation

All four are valid 32-bit little-endian ELF (`7f 45 4c 46 01 01 01 00`):

| file | bytes | PT_LOAD | filesz |
|---|---|---|---|
| `a640_zap.mdt` | 6860 | 1 | 1968 |
| `a640_zap.b00` | 148 | 1 | 1968 |
| `a640_zap.b01` | 6712 | 0 | — |
| `a640_zap.b02` | 1968 | 0 | — |

Total `memsz` across the set: **3936 bytes (0xf60)**.

That number matters at bring-up time: upstream's `zap_shader_load_mdt()`
computes `mem_size = qcom_mdt_get_size(fw)` and returns `-E2BIG` if it exceeds
the reserved region, so the DT `zap-shader` reserved-memory node only needs to
be a few KB.

## Why Samsung-signed matters

These are signed by the device's own key, which is the whole point of the
zap investigation: TrustZone on Samsung silicon rejects the Qualcomm
*test-signed* `a640_zap` that `linux-firmware` ships, leaving the GPU unable to
leave secure mode. So the cheap fallback (apt-install `linux-firmware` and use
the upstream blob) is no longer needed as a separate experiment — the real
artifact is in hand.

## Still outstanding (not started)

1. **DT**: no `zap-shader` child node exists under the live GPU node, and no
   `zap-shader` reserved-memory region exists in `sm8150.dtsi:578`. The existing
   `pil_gpu: qcom,kgsl-hyp` node is a *sibling* of `msm_gpu` under `&soc` and
   has no `reg`/`memory-region`, and is not present in the running DTB at all.
2. **Driver**: no `a6xx_zap_shader_init` / `adreno_zap_shader_load` exists for
   a6xx. `hw_init` unconditionally does
   `gpu_write(REG_A6XX_RBBM_SECVID_TRUST_CNTL, 0)` instead of upstream's
   three-way branch. The a5xx implementation
   (`drivers/gpu/drm/msm/adreno/a5xx_gpu.c:29-95`) is a near-complete twin of
   upstream's and is the natural template — it needs the a5xx→a6xx move plus
   two additions upstream has and it lacks: a `qcom_pas_is_available()` check,
   and reading `firmware-name` from the DT node (the device-specific signed
   name, which is exactly the mechanism that matters here).

Both were deliberately left undone pending review — the DT edit is boot-critical
on a device whose boot chain was only just recovered.

## Caveat still standing

A blanket "GPU stuck in secure mode" does not fully explain the symptom: the
fence echo works, so the CP *can* write non-secure DDR. Unless SECVID filtering
is asymmetric between the CP and the RB/UCHE path, zap alone would not account
for "CP writes land, RB writes never do".