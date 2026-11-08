# Firmware comparison: both leads closed — and a new one found in the process

Date: 2026-10-18

## Lead 1: is the GMU firmware the same build the KGSL reference boots?

**Yes — it is the stock one, and it has never been swapped.**

| file | device (`/usr/lib/firmware/qcom/`) | size | note |
|---|---|---|---|
| `a640_gmu.bin` | `a640_gmu.bin` | 37680 B | **stock** — same size/date as the `apnhlos` copy at `/home/usb/mnt-sda/a6xx-build/fw/gpu-fw/a640_gmu.bin` |
| `a630_sqe.fw` | `a630_sqe.fw` | **34188 B** | newer upstream linux-firmware build, installed by the project |
| | `a630_sqe.fw.orig` | 32304 B | **stock** — still present |

`apnhlos` (sda16, vfat) is where the stock GPU firmware bundle lives
(`/mnt/image/a640_gmu.bin`, `a630_sqe.fw`, `a640_zap.*`).

`ANDROID-KGSL-PSP-REFERENCE.md` already verified the *running* firmware under
KGSL: "ITCM+0x24 remains exactly `0xDEADBEEF` after a KGSL boot", the ITCM
is "identical to static image", and "DTCM base matches our tree's contents all
but one word". So the GMU firmware we load is the reference's firmware.

### The SQE ucode is properly ruled out

`FINDING-SQE-FIRMWARE-DELTA-REFUTED.md` is a valid **two-point** test, not a
one-point one:

| SQE ucode | result |
|---|---|
| stock 32304 B (dword[1] version `0x016ee190`) — the image-263 baseline | T1 256/256 intact, T2 1024/1024 intact, render 0/6 |
| newer upstream 34188 B (version `0x016ee207`) | **byte-identical dead behaviour** |

Two different ucode builds, same result ⇒ the SQE microcode is not the cause.
That conclusion stands.

**Note for the record:** the *reference* that boots cleanly used the **stock**
SQE ucode, and the device currently has the **newer** one installed. The
two-point test makes that irrelevant to the rendering defect — but it is worth
restoring the stock blob at some point so the device matches the reference
exactly, since the crash axis is still unexplained and the SQE ucode is part of
the GMU's environment.

## The new lead: the a6xx path programs NEITHER trust mechanism

Comparing the a5xx path (in the same tree) against the a6xx path exposed this.

**a5xx** (`a5xx_gpu.c:490-539`) does two things, and chooses between them:

```c
	gpu_write(gpu, REG_A5XX_CP_PROTECT_CNTL, 0x00000007);
	... CP_PROTECT(0..17), including the UCHE block ...
	/* Content protect */
	gpu_write(gpu, REG_A5XX_CP_PROTECT(6),
		ADRENO_PROTECT_RW(REG_A5XX_RBBM_SECVID_TSB_TRUSTED_BASE_LO, 16));
	gpu_write(gpu, REG_A5XX_CP_PROTECT(7),
		ADRENO_PROTECT_RW(REG_A5XX_RBBM_SECVID_TSB_TRUST_CNTL, 2));
	...
	gpu_write(gpu, REG_A5XX_RBBM_SECVID_TSB_CNTL, 0);
	/* Disable the trusted memory range - we don't actually supported secure
	 * memory rendering ... and we don't want to block off part of the
	 * virtual memory space. */
	gpu_write64(..., TSB_TRUSTED_BASE_LO ..., 0x00000000);
	gpu_write(..., TSB_TRUSTED_SIZE, 0x00000000);
```

and it has a **fallback path** — `a5xx_gpu.c:582-596`:

> *"guessed wrong then access to the RBBM_SECVID_TRUST_CNTL register will …
> **"Zap shader not enabled - using SECVID_TRUST_CNTL instead"**"*

i.e. **a5xx has two ways to establish the trust configuration: the ZAP shader,
or SECVID_TRUST_CNTL.**

**a6xx in this tree has NEITHER:**

1. **The ZAP shader is never hooked up.** The tree's own probe says so:
   `SECVID134: mode=2 ZAP-UNAVAILABLE (no a640_zap fw, no zap-shader DT
   child, no a6xx hookup)`.
   Yet the ZAP firmware *is* present and *is* named:
   - on disk: `/usr/lib/firmware/qcom/a640_zap.{mdt,b00,b01,b02}`
     (extracted from `apnhlos`, installed 2026-10-14),
   - in the DT: `sm8150-gpu.dtsi:17  qcom,firmware-name = "a640_zap";`
   - in the driver: `adreno_device.c:115  .zapfw = "a640_zap.mdt";`
   - but the a6xx path never loads or starts it.

2. **The `SECVID_TRUST_CNTL` write is skipped.** `a6xx_gpu.c`:
   ```c
   if (secure_probe == 1 || experimental_nonlegacy) {
           pr_info("SECVID134: mode=%d/nonlegacy=%d SKIP direct SECVID_TRUST_CNTL write\n", ...);
   } else {
           gpu_write(gpu, REG_A6XX_RBBM_SECVID_TRUST_CNTL, 0x0);
   }
   ```
   and our cmdline has `a6xx.experimental_nonlegacy=1`, so it is always skipped.

The live values confirm the result: `TRUST_CNTL=0x1 TSB_CNTL=0x0 TSB_BASE=0x0 SIZE=0x0`.
(The `SECVID_TRUST_CNTL` write happens to be a no-op, since the value is already
`0x1` — but the **ZAP path is the one that is entirely absent**.)

**This is candidate D — the "secure gate"** — and the instrument used to dismiss
it (`CP_REGISTER_PROTECTION_ERROR`, never observed set) **structurally cannot
see a fault that happens in the hypervisor**. The a6xx path is the only path in
the tree that establishes *no* trust configuration at all.

## So the two leads stand as

| lead | status |
|---|---|
| 1. GMU/SQE firmware build | **closed** — GMU is stock and matches the reference; SQE ruled out by a valid two-point test |
| 2. a UCHE-side observation | **open** — nothing the project can read today is UCHE-side |
| **3. (new)** the a6xx trust configuration is unprogrammed | **open, and it is the "secure gate" candidate D, re-opened on a mechanism argument rather than a guess** |

## Next

The a5xx tree contains the a5xx ZAP hookup that the a6xx path lacks. The
experiment is then to **port the a5xx ZAP path to a6xx**: load
`a640_zap.mdt` (+ the .b0x segments), start it, and let it establish the trust
configuration — and then re-read `SECVID_TRUST_CNTL` / `TSB_*` to see whether
the values change.

Everything is already on disk and named; what is missing is the a6xx hookup, which
is exactly the "no a6xx hookup" the probe reports.
