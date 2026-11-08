# Image 243 — a640 bring-up parity with CAF + mainline (5 runtime-toggleable items)

Image: `artifacts/flash/243-halium-boot-a6xx-parity.img`
sha256 `18f73e9028d81a48f628c1a2042408ff5d03d6706a0332236a35af33dc42efc9`
Kernel `#50`, image size 52543488 (identical to 242).

**Single-variable guarantee:** the boot header is byte-identical to image 242
except the sha1 `id` field (offset 576..607), which must change because the
kernel changed. Verified by `/tmp/opencode/pack243.sh`, which refuses to pass
if any header byte outside `id` differs. So the ONLY functional delta from 242
is the kernel.

## Why now

`ORACLE-PHASE0-RESULTS.md` settled the measurement question: on a render
target the CPU never writes, with the read path independently proven, the
buffer is genuinely all-zero after the GPU retires the blit. The corpus's
finding stands and the remaining suspects are register-level divergences.

Every item below is a write that **CAF and mainline both perform and this tree
does not**, or does differently. Values are a640-specific in both references.

## Changes

### `a6xx.xml.h` — two defines fixed (not written before, but landmines)

| define | was | now | vendor |
|---|---|---|---|
| `REG_A6XX_RBBM_GPR0_CNTL` | `0x60` | `0x18` | `a6xx_reg.h:416` |
| `REG_A6XX_RBBM_NC_MODE_CNTL` | `0x14d0` | `0x534` | `a6xx_reg.h:403` |

Both were still x4-inflated after the image-242 fix, which only covered the
GBIF block. The second one mattered: `RBBM_NC_MODE_CNTL` was an open
implementation target, and writing it before this fix would have landed on an
unrelated address.

### 1. `a640_roq2` (default **on**) — `CP_ROQ_THRESHOLDS_2`

`0x010000c0` → `0x02000140`.

CAF `adreno_a6xx.c:470-472` gates `0x02000140` on
`adreno_is_a640_family || adreno_is_a650_family`, with `0x010000C0` in the
`else` (a630) branch. Mainline `hw_init()` is identical. The tree was running
the a630 value on an a640. ROQ is the render output queue.

### 2. `a640_gbif_halt` (default **on**) — GBIF halt clear, write-only

Replaces `HWINIT[03] GBIF/HALT: SKIPPED (image 233)`.

Images 231/232 tried this and both runs were lost. 231's "read hangs" result
was measured at a **garbage address** — `GBIF_HALT` was `0xf114` back then
(4x-inflated, unmapped), not `0x3c45`. The inference "GBIF does not answer
reads" was therefore unfounded, and the write was dropped along with the
diagnostic, leaving the register unprogrammed for 231..242.

Both references clear it for GBIF parts: mainline writes `GBIF_HALT=0` +
readback then `RBBM_GBIF_HALT=0` + readback as the first thing in `hw_init()`;
CAF does a halt→poll-ACK→unhalt handshake in `adreno_smmu_resume()`. We do the
bare writes with **no readback**, since the readback is what wedged the GPU in
231 and carries no information this test needs.

### 3. `a640_gmu_cgc` (default **on**) — GMU always-on clock gate

Closes the corpus's own open item "GMU AO CGC registers (defines present, no
writer)".

CAF `__get_gmu_ao_cgc_*` / `__get_gmu_wfi_config()`, a640 taking the `else`
branch of each selector:

```
AO_GMU_CGC_MODE_CNTL  = 0x00020202
AO_GMU_CGC_DELAY_CNTL = 0x00010111
AO_GMU_CGC_HYST_CNTL  = 0x00005555
CX_GMU_WFI_CONFIG     = 0x00000002   (a620/a640/a650 explicitly)
```

Mainline `a6xx_set_hwcg()` writes the same three (`cgc_delay = 0x10111`,
`cgc_hyst = 0x5555` for anything not a612/a615) — identical to CAF for a640.
`WFI_CONFIG` was missing entirely.

### 4. `a640_qsb` (default **off**) — stop applying an a610 sequence to a640

`GBIF_QSB_SIDE0..3 = 0x00071620` was being written on the a640 path. Mainline
applies that sequence to `adreno_is_a610_family()` **only**. CAF never writes
it in `adreno_a6xx.c`. Default flipped off; the readbacks were also removed
(they were the same class of read that wedged GBIF in 231).

### 5. `a640_rbbm_nc` (default **on**) — `RBBM_NC_MODE_CNTL = 1`

Mainline `a6xx_set_ubwc_config()` writes
`qcom_ubwc_macrotile_mode(cfg)`, which for UBWC >= 2.0 is
`cfg->highest_bank_bit`. The d2q DT sets `qcom,ubwc-mode=<3>` and
`qcom,highest-bank-bit=<15>`, so the value is `1`.

## Runtime A/B — one flash, five experiments

All five are `0644` module params, so each can be toggled without a rebuild
and re-triggered:

```
/sys/module/msm_drm/parameters/a640_roq2
/sys/module/msm_drm/parameters/a640_gbif_halt
/sys/module/msm_drm/parameters/a640_gmu_cgc
/sys/module/msm_drm/parameters/a640_qsb
/sys/module/msm_drm/parameters/a640_rbbm_nc
```

If parity does not fix rendering, flip them one at a time and re-trigger to
attribute the result. The corpus has spent 242 images on one-variable-per-flash
cycles; with 28 submits per run there is no reason to spend a flash per bit.

## Expected log lines

```
GBIF243: GBIF_HALT=0 RBBM_GBIF_HALT=0 written (no readback)
GBIF[00-03] QSB_SIDE0..3 SKIPPED (a640_qsb=0)
ROQ243: THRESHOLDS_2=0x02000140 THRESHOLDS_1=0x8040362c a640_roq2=1
RBBMCNC243: NC_MODE_CNTL=0x1 written=1
GMUCGC243: MODE=0x... DELAY=0x10111 HYST=0x5555 WFI=0x2 state=1
HWINIT[25] return ret=0
```

## Readout

- **Non-zero pixels in `oracle.py` round 0** → one of these five was it. Flip
  them to attribute.
- **Still zero, bring-up still clean** → all five CAF/mainline-attested
  divergences are eliminated, and the GPU-zap / secure-mode path
  (`a640_zap` from stock `vendor.img`, then `CP_SET_SECURE_MODE 0` instead of
  poking `SECVID_TRUST_CNTL`) becomes the next image. Note that no
  `a640_zap` exists in the vendor `/firmware` partition — the extraction is
  still to be done.

## Packaging recipe (derived this session, worth keeping)

`/tmp/opencode/pack243.sh`. Three traps, all of which produce a subtly wrong
image rather than an error:

1. `/usr/bin/mkbootimg` defaults `--base 0x10000000`, moving `kernel_addr`
   from `0x8000` to `0x10008000`. Every d2q image uses base 0.
2. The Halium `mkbootimg`
   (`n9750-halium/out/host/linux-x86/bin/mkbootimg`) is a different binary
   from `/usr/bin/mkbootimg` and is the one that built these images.
3. Image 242's cmdline is 535 chars in a 512-byte field. The original builder
   did not bound-check and let it **overflow into the `id` field at offset
   576**, so the trailing `d=1 a6xx.uche_gbif_rw=1` was never delivered to the
   kernel at all. The script truncates to exactly 512 to reproduce 242's
   effective kernel-visible cmdline.

Method: validated by reproducing 242's header byte-for-byte outside `id`
before building 243. `pack243.sh` re-runs that check on every build.