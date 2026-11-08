# Image 243-v2 — parity changes refuted for rendering; GBIF_HALT write freezes the machine

Image: `artifacts/flash/243-halium-boot-a6xx-parity-v2.img`
sha256 `d3106df16605236a4a9c1a2fd6d617d1b9f8c4fb3231f462b040c88dcbafe599`
Kernel `#50`. Ramdisk sha `eb5d2eb5…` verified (the original 243 had a corrupt
ramdisk — see `IMAGE-243-FAILED-BOOT.md`).

The first 243 attempt never reached Debian; that was entirely my packaging
error. This entry is the first real test of the parity changes.

## Headline: two clean results, both by runtime A/B on one image

### 1. `GBIF_HALT` clear at the corrected address FREEZES the machine

| `a640_gbif_halt` | trigger result |
|---|---|
| `1` | system froze immediately; needed a forced reset (`UploadCause[User press reset keys for 7 sec]`, `PMIC abnormal reset`) |
| `0` | GPU came up cleanly, `HWINIT[25] return ret=0` |

Same image, same boot method, one sysfs write between the two runs.

This **confirms the image-231 observation was real**, for the wrong stated
reason. In 231/232 the define was `0xf114` — 4x-inflated and unmapped — so the
write was a no-op and the "GBIF does not answer reads" conclusion was drawn from
a garbage address. Now that the address is `0x3c45`, the write takes effect and
wedges the AXI bus. Both CAF and mainline clear this register, so this is a
CAF/mainline divergence we cannot adopt on this hardware.

**Default is now OFF** in the source.

### 2. The other parity changes do not fix rendering

With `a640_gbif_halt=0` and the remaining four active, bring-up is clean and
every write verified:

```
GMUCGC243:   MODE=0x20202 DELAY=0x10111 HYST=0x5555 WFI=0x2 state=1
ROQ243:      THRESHOLDS_2=0x02000140 THRESHOLDS_1=0x8040362c a640_roq2=1
RBBMCNC243:  NC_MODE_CNTL=0x1 written=1
RINGHW:      rb_iova=0x700001000 rb_size=32768
SELFTEST142: done last_iter=10 ALL 10 PASS
HWINIT[25]   return ret=0
```

The cache-sound oracle (`/root/oracle.py`, from `ORACLE-PHASE0-RESULTS.md`)
still reports:

```
--- round 0: cleared RED ---
  bo 64x64 stride=256 fd=7 inode=39174  FRESH (sound)
  pixel[0,0]     = 0x00000000 (r=0 g=0 b=0 a=0)
  nonzero pixels = 0 / 4096
--- Test C: instrument self-check ---
  inode=37694 fd=10 wrote 0xCDCDCDCD, read back 0xcdcdcdcd -> READ PATH OK
```

So `CP_ROQ_THRESHOLDS_2`, the GMU AO CGC block, `RBBM_NC_MODE_CNTL` and
`WFI_CONFIG` are all **exonerated**. They are correct values and they are now
in the tree, but none of them is the missing write.

## Where the standing evidence now points

Everything CAF and mainline do that a640 needs, within reach of `hw_init`, has
been applied or ruled out:

- SMMU translation — real, verified (28 GiB IOVA on 11 GiB RAM)
- LLC / SCID / slice activation / domain attr / map hint — image 242, refuted
- UBWC amsbc, `RBBM_NC_MODE_CNTL` — 237 / this image, refuted
- A640 clock values, `RBBM_CLOCK_CNTL` — 235, refuted
- `GBIF_QSB_SIDE0..3` — was an a610 sequence, removed
- `GBIF_HALT` — now known to be actively harmful on this part
- `CP_ROQ_THRESHOLDS_2` a640 value — applied, refuted

Remaining from the plan: the **GPU zap / secure-mode path**. Our tree has no
`a6xx_zap_shader_init` / `adreno_zap_shader_load` at all — those exist only for
a5xx (`a5xx_gpu.c:29-87`) — and a6xx unconditionally pokes
`REG_A6XX_RBBM_SECVID_TRUST_CNTL = 0` instead of issuing `CP_SET_SECURE_MODE 0`
after authenticating a zap shader. TrustZone on Samsung silicon rejects the
Qualcomm test-signed `a640_zap` from `linux-firmware`, which is a documented
Samsung-specific failure (gts6l-mainline-linux, SM8150P/A640).

Blocker: **`a640_zap` is not in the vendor `/firmware` partition** (only
`a640_gmu.bin` and `a630_sqe.fw` are), and the stock ROM tar contains only the
same images already extracted. It is most likely in the modem filesystem
(`/firmware` in `modemst1/2`), which has not been pulled.

Residual caveat against zap: if the GPU were stuck in secure mode the CP's
non-secure DDR writes would fail too, and the fence echo demonstrably works. So
secure mode alone does not explain the CP-works/RB-doesn't split, unless the
filtering is asymmetric between the CP and the RB/UCHE path.

## Open loose end

`gbm` returns the **same dmabuf inode** for all three swap rounds
(`inode=39174`, different fds). `eglSwapBuffers` is not cycling distinct
buffers on this stack. Any multi-colour test depends on getting fresh bos, so
this is worth a look independently of rendering — it may itself be a symptom of
the swap path not completing.

## Source state

The tree was fully reverted during the rollback exercise and has now been
re-applied to match the running kernel, with `a640_gbif_halt` defaulted OFF.
Files restored from `/tmp/opencode/*.pre243` were `touch`ed before editing —
`cp -a` preserves mtimes and makes make skip recompilation, which silently
produced a stale binary earlier.