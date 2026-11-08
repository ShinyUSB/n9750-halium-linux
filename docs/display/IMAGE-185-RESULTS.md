# Image 185 — results. The probe is exonerated; the *method* was the bug.

Kernel `#127`, flashed clean. Phone booted healthy: cpstate node present,
`watchdog_v2.enable=0`, WDT never armed.

## What actually happened

Three separate things, and only the first was interesting.

### 1. The probe works, but fired too early to matter

The log shows the node is live and correctly refuses to act before the GPU
exists:

```
[91.692778] [render185.sh: 1164] a6xx: cpstate: no GPU attached yet (write a6xx_gpu_load first)
[91.693020] [render185.sh: 1164] a6xx: on-demand load handed to a6xx-load thread
[91.693027] [a6xx-load: 1171]   a6xx: on-demand GPU core load requested
```

`render185.sh` dumped "at rest" *before* writing to `a6xx_gpu_load`. At that
moment `priv->gpu` is legitimately NULL, because this kernel attaches the GPU
on demand. So the at-rest dump could never have produced anything useful — it
was guaranteed to bail. A design error, not a driver error.

### 2. The wedge is NOT the probe

This is the important result. Counting through the archive for that boot:

| marker | count |
|---|---|
| `SELFTEST142: done` | 1 (bring-up **won**) |
| `POSTINIT[40]` | 1 (hw_init completed) |
| `egl6` | **0** |
| `GL_RENDERER` | **0** |
| `eglInitialize` | **0** |
| `CPDUMP` | 0 |

**Mesa never started.** The phone died after a *successful* bring-up, with no
GL submit outstanding and the probe having already bailed without touching a
single GPU register. The thing I was worried about — reading MMIO during a
blocked submit — never happened.

So image 185 is not a regression. `init_lock` was hoisted from a
function-local `static DEFINE_MUTEX` to file scope, which is the same single
instance with the same static initialisation; the probe existed but was called
exactly once and returned early. There is no plausible mechanism by which 185
behaves differently from 184. **This is the usual nondeterministic PCIe/network
stall that has followed every GPU load all evening, and this time it did not
self-clear.** Two earlier renders caused the same link loss and did recover.

Conclusion recorded so it is not re-litigated: do not attribute the wedge to
the probe without evidence that the probe actually ran.

### 3. IRQ numbers are assigned per boot — do not hardcode them

`cp185.sh` grepped `/proc/interrupts` for lines `486:`-`489:`, copied from the
earlier boot. On a later boot line 486 is:

```
486:  0  0  ...  PDC-GIC 520 Edge  dm_hs_phy_irq
```

`dm_hs_phy_irq`, not the GPU. The `spi 300 → "PDC-GIC 332"` mapping is real
(it is how the earlier boot's 487 was confirmed as ours), but the **Linux IRQ
number is not stable across boots.** Both shell scripts now match on device
name (`A640|kgsl|gmu`) instead. Same class of mistake as the one that made me
read the surfaceless-vs-GBM conclusion wrongly at first.

## The real lesson, and what 186 changes

The failure mode of this project is *losing the link*. So **any diagnostic that
requires an ssh round-trip is a diagnostic we will not collect** — exactly
what happened: the probe needed a write at a precise moment, over a link that
dies whenever the GPU is loaded, so it never fired when it mattered.

Image 186 removes the dependency entirely. `load_gpu_thread()` — the kthread
already proven to survive a wedged bring-up — now takes the dump itself,
twice, with no shell and no timing luck:

- `CPDUMP[auto-rest]` immediately after the bring-up, GPU idle
- `CPDUMP[auto-post]` 12 s later, by which point an `egl6.py` run has
  submitted and is blocked in `glFinish`

Both land in the kernel ring buffer, which crashwatch archives on the next
reboot. **The data now survives the link dying**, which is the one thing this
project can count on happening.

The sysfs node stays as a manual trigger, but it is no longer load-bearing.

One flash, one lottery ticket, one `egl6.py` run — and the answer arrives in
the archive whether or not we keep the phone reachable.