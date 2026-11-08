# Image 210 — make GPU bring-up strictly once per boot

Date: 2026-11-11. Built, packaged, **not yet flashed**. Two guards, both defensive.

## Why

Two hard freezes on image 209, both costing a manual reboot and producing **no
diagnostics** (the harness could not reach the phone to collect any).

The common factor is a **second `hw_init` on an already-live GPU**:

```
[  60.250383] bash:1151  trigger 1 -> hw_init ret=0 -> CPDUMP auto-post end  (clean)
[ 140.391859] bash:1217  trigger 2 -> RINGHW[05] a6xx_cp_init enter -> nothing more
```

Freeze A: trigger 2 stopped dead after `a6xx_cp_init enter`. Log simply ends,
no panic, no oops.

Freeze B (second attempt, single trigger from my side yet two handoffs on
the boot): trigger 2 completed `hw_init returned (ret=0)` and the machine still
hung later. **So the wedge is not reliably inside `a6xx_cp_init`** — re-running
hw_init corrupts live state and the machine dies somewhere downstream.

## Root cause of the re-entry

`adreno_load_gpu()` allocates a **brand new** `msm_gpu` on every call
(`a6xx_gpu_init` → `kzalloc`). The new object's `needs_hw_init` is then set to
true by the `pm_runtime_get_sync()` → `msm_gpu_pm_runtime_resume()` path
(`msm_gpu.c:176`). Result: a full `adreno_hw_init()` executed against a GPU that
is already programmed — reprogramming `RB_BASE`/`RB_CNTL`, re-running
`a6xx_cp_init`, re-enabling the SQE on top of live state.

The existing `if (!priv->gpu)` guard in `load_gpu_real()` does not prevent this,
and `priv->gpu` cannot be relied on: on freeze A, trigger 1 completed cleanly and
trigger 2 ran the whole bring-up anyway.

## The two guards

### 1. `msm_drv.c` — bring-up is one-shot at the source

```c
static bool gpu_load_done;

if (gpu_load_done) {
	pr_info("a6xx: REFUSING repeat bring-up (already attempted this boot)\n");
	mutex_unlock(&init_lock);
	return;
}
gpu_load_done = true;
```

Set on the **first attempt whether it succeeded or not**, so the trigger is
genuinely one-shot. Bring-up has been reliable for many consecutive boots and a
failed first attempt has never been usefully retried on this tree, so there is
nothing legitimate to protect by allowing a retry.

### 2. `msm_gpu_hw_init()` — defence in depth

```c
if (gpu->hw_init_done) {
	pr_info("A6XX_TRACE: msm_gpu_hw_init REFUSING re-init ...\n");
	gpu->needs_hw_init = false;
	return 0;
}
```

The `needs_hw_init == 0` test already there is **not** sufficient protection,
because `needs_hw_init` is set back to true by `msm_gpu_pm_runtime_resume()`
(`msm_gpu.c:176`) and `a6xx_pm_resume()` (`a6xx_gpu.c:1539`). Any path that takes
a pm_runtime reference on an already-programmed GPU re-arms it.

`hw_init_done` is a new `bool` in `struct msm_gpu`:

| site | action |
|---|---|
| `msm_gpu_hw_init()`, on success | `hw_init_done = true` |
| `msm_gpu_hw_init()`, guard | refuse and clear `needs_hw_init` |
| `a6xx_pm_suspend()` | `hw_init_done = false` |

Clearing on suspend means a genuine suspend/resume cycle can still legitimately
re-initialise, while accidental re-entry is refused. That clear carries **no
printk**, deliberately — images 179–181 established that a printk on the
suspend path can wedge the caller inside `console_unlock`.

## What this image still measures

The image-209 diagnostics are **kept**, because they have still never produced
data (`RINGCPU = 0` on both 209 attempts — the run never reached a submit):

- `RINGCPU` — ring dwords from the CPU side, first 3 submits
- `RINGHW[04..06]` — `memprs_iova`, `rbmemptr(rptr/fence)`, aspace vs GMU domain

## Procedure for the run

The measurement is still the point of this image, so:

1. Flash, boot, **one** trigger in its own short command — no `sleep` long enough
   to hit a tool timeout, and no retry.
2. Confirm `BABEFACE`.
3. Run `fbprobe.py` to force a real submit.
4. Collect `RINGCPU` from dmesg **before anything can reboot**.

Gate: `GL_RENDERER == FD640` before any stage result counts.

## If it still freezes

The guard proves the re-entry path is closed, so a freeze with
`a6xx: REFUSING repeat bring-up` present means the cause is *inside the first*
bring-up and the guards have told us that much for free. `hw_init_done` also
gives a definitive answer to a question we could not previously ask: whether the
second bring-up was even reaching the same GPU object.

## Verification

- Build exit 0, no new warnings.
- All three `hw_init_done` sites present (`msm_gpu.h:141`, `msm_gpu.c:267/302`,
  `a6xx_gpu.c:1639`).
- `RINGCPU` diagnostics retained (3 sites).
- Byte-identical to 209 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/210-halium-boot-a6xx-initguard.img
sha256 fdae378d61dd5daddb3295b352a52d67925f10bcc189839963d50b5557ca49b0
```

## Not changed

Images 187–209 untouched. The 209 measurement code is kept. No behaviour
changes other than refusing to re-initialise a live GPU.
