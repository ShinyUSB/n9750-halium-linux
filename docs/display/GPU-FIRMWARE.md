# A640 firmware archaeology

Source: Samsung stock firmware `SAMFW.COM_SM-N9750_TGY_N9750ZSU6HZE1_fac.zip`
(6.3 GB, Odin package) for SM-N9750 / SM8150.

This file supersedes the earlier claim in `A6XX-FEASIBILITY.md` that no Samsung
GPU firmware is available locally. **That claim was wrong** - the package *is*
decompressable, as a `zip` -> `*.tar.md5` -> partition images -> ext4 chain.

## Unpacking chain (verified)

```
SAMFW.COM_SM-N9750_TGY_N9750ZSU6HZE1_fac.zip
  -> BL_....tar.md5     77,987,953   (plain tar)
  -> AP_....tar.md5     7,613,307,003 (plain tar)
  -> CP_....tar.md5     49,264,749   (NOT a plain tar - Odin-specific)
  -> CSC_/HOME_CSC_....tar.md5

AP_....tar.md5 contents:
  boot.img.lz4             24,016,427
  vendor.img.ext4.lz4     845,944,307   <-- GPU firmware lives here
  system.img.ext4.lz4   4,025,763,807
  userdata.img.lz4     1,557,893,405
  ...
```

Commands used:

```sh
unzip -p "$Z" AP_...tar.md5 | tar -xOf - vendor.img.ext4.lz4 > vendor.img.ext4.lz4
lz4 -d -f vendor.img.ext4.lz4 vendor.img          # 845 MB -> 1,279,943,056 B
simg2img vendor.img vendor.raw.img                # sparse -> 1.6 GB ext4
debugfs -R "dump /firmware/a630_sqe.fw out" vendor.raw.img
```

## Found: A6xx GPU firmware (all present, verified)

Extracted from `/firmware` in the vendor partition. Directory listing also shows
`a612_rgmu.bin`, `a618_gmu.bin`, `a630_gmu.bin` alongside the two we need.

| file | size | sha256 |
|---|---|---|
| `a630_sqe.fw` | 32,304 | `a0e1b583f620fabe32729ce367959d1960638663244d7d0cfc21b9a5215a018b` |
| `a640_gmu.bin` | 37,680 | `3ff0c02708bbe78641db887fa62f3a7f9337934d0c2ce0b961ef7c43172591d2` |
| `a630_gmu.bin` | 32,768 | `b07f90299ff2ae33948fcfd6ed67f53fbd287b5bf85ff2d43db11d4d32c1621f` |

### Format notes

`a630_sqe.fw` begins with **four zero bytes** before the code
(`00000000 90e16e01 0b1f0001 ...`). That is consistent with upstream's
`adreno_fw_create_bo()`, which does `memcpy(ptr, &fw->data[4], fw->size - 4)` -
i.e. it deliberately skips a 4-byte header. The Samsung blob therefore has the
same layout the upstream a6xx code expects, and can be used as-is.

`a640_gmu.bin` begins `00000000 30000000 deadbeef deadbeef ...` and contains
`deadbeef` / `efbeadde` markers again at offset 0x38, i.e. a header with fields
patched at load time. From offset 0x14 onward its code is identical to
`a630_gmu.bin`, which is expected: the A630 and A640 GMU firmware are the same
code with different header parameters.

`a6xx_gmu.c` loads the GMU blob raw (`image = (u32 *)adreno_gpu->fw[ADRENO_FW_GMU]->data`)
and enforces `size <= 0x8000`. `a640_gmu.bin` is 0x9300 = 37,680 bytes, which is
**larger than 0x8000**. The upstream sanity check would reject it:

```c
if (adreno_gpu->fw[ADRENO_FW_GMU]->size > 0x8000) {
        dev_err(&gmu->dev, "GMU firmware is bigger than the available region\n");
```

This is a real, concrete runtime blocker to record - not a theoretical one. The
GMU region must be enlarged, or the check relaxed, before the Samsung GMU blob
can be used as-is.

## Not found: ZAP firmware

There is **no `a640_zap*` anywhere in the AP vendor filesystem.** The whole
`/firmware` listing was enumerated and contains only:

```
CAMERA_ICP.elf  a612_rgmu.bin  a618_gmu.bin  a630_gmu.bin
a630_sqe.fw     a640_gmu.bin   <wifi/audio/nfc/ipa blobs>
```

The bootloader partition (`BL_...tar.md5`) contains `tz.mbn`, `hyp.mbn`,
`cmnlib.mbn`, `km4.mbn`, `dspso.bin`, `NON-HLOS.bin` - no ZAP image either.

This matches the device tree, which loads ZAP through TrustZone/PIL rather than
through the filesystem:

```dts
pil_gpu: qcom,kgsl-hyp {
        compatible = "qcom,pil-tz-generic";
        qcom,pas-id = <13>;
        qcom,firmware-name = "a640_zap";
};
```

So on this device the ZAP is provisioned inside the secure world by the
bootloader and is not exposed as a file that Linux can load.

### Consequence for upstream a6xx

Upstream loads the ZAP itself, via `request_firmware()` + `qcom_mdt_load()` +
`qcom_pas_auth_and_reset()`, expecting e.g. `qcom/a640_zap.mdt` (+ `.b00`,
`.b01`, `.b02`). That file does not exist for this device in any local source.

Options, in order of preference:
1. Take `qcom/a640_zap.mdt` from upstream `linux-firmware`. Untested here;
   requires the TrustZone PAS auth at `qcom_pas_auth_and_reset()` to accept it.
2. Extract from a secure partition with an unlocked bootloader. Requires
   unlocking, which is out of scope for this milestone.
3. Disable the ZAP path for the compile-only milestone (it is not needed to
   link or to compile).

## Firmware state for the compile-only milestone

| blob | needed to compile | needed to run | available |
|---|---|---|---|
| `a630_sqe.fw` | no | yes | **yes**, extracted + hashed above |
| `a640_gmu.bin` | no | yes | **yes**, but size exceeds upstream's 0x8000 check |
| `a640_zap.mdt` | no | yes | **no** - not in this firmware package |

Nothing here blocks the compile-only milestone. The SQE/GMU size check and the
absent ZAP are runtime concerns recorded for M16.

## Licensing / provenance

All three blobs are proprietary Qualcomm firmware redistributed by Samsung inside
a licensed firmware image. They are **not** part of the open-source kernel
change and must be kept in a separate directory from the source tree. They are
held here only as extracted artefacts for testing; provenance is Samsung stock
firmware for SM-N9750, build `N9750ZSU6HZE1`, `MQB109611235`, dated 2026-05-18.