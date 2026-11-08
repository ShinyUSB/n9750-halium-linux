# Image 246 — zap loader works and the zap RUNS. Rendering is still broken.

Kernel with `subsys-pil-tz.c` changes. **No DT change, no new firmware, no
GPU driver change** — the zap was already 95% wired up.

## 1. A real latent bug: `subsys_desc.fw_name` was never populated

`drivers/soc/qcom/subsys-pil-tz.c` reads the DT firmware name into the wrong
field:

```c
rc = of_property_read_string(pdev->dev.of_node, "qcom,firmware-name", &d->desc.name);
```

but `subsys_powerup()` consumes a *different* field:

```c
static int subsys_powerup(const struct subsys_desc *subsys)
{
	d->desc.fw_name = subsys->fw_name;   /* subsys_desc.fw_name */
	ret = pil_boot(&d->desc);
```

`subsys_desc.fw_name` (`char[256]`, `devm_kzalloc`'d → empty string) was
**never assigned by any probe in the tree**. `grep -rn "\.fw_name" drivers/`
showed only three readers (`subsys-pil-tz`, `subsys-pil-bg`,
`pil-q6v5-mss`) and zero writers. So any powerup would have run

```c
snprintf(fw_name, sizeof(fw_name), "%s.mdt", desc->fw_name);   /* ".mdt" */
```

and requested a firmware literally named `.mdt`.

**Fixed** by copying the DT name into `subsys_desc.fw_name` at probe. Proven
working: the load then requested `a640_zap.mdt`, exactly as intended.

> Still unexplained: how does *venus* get a correct name? Its log says
> `Failed to locate venus.mdt` — so `desc->fw_name` was `"venus"` there. No code
> path assigns it. I did not fully close this; the fix is correct regardless,
> but a second mechanism may exist.

## 2. On-demand load, off by default

`pil_boot()` asks the secure world to load and start a PAS. Doing that
unattended at probe is how you wedge a machine, so the probe deliberately does
not. Instead each node gets:

```
echo 1 > /sys/devices/platform/soc/soc:qcom,kgsl-hyp/fw_load
```

which runs the normal `subsys_powerup()` → `pil_boot()` path and prints
`SUCCEEDED`/`FAILED` with the rc. Nobody writes it, so boot behaviour is
unchanged apart from the `fw_name` fix.

**Boot of 246 was verified clean**: no `fw_load` activity, venus baseline
unchanged (3 failures), GPU status `unsupported` (untouched).

## 3. The zap now loads and runs

First attempt failed on path only:

```
firmware a640_zap.mdt: _request_firmware_load: firmware state wait timeout: rc = -2
```

`request_firmware` searches `/lib/firmware/`, **not** `/lib/firmware/qcom/`
(no `fw_path` override), fell through to the absent usermode helper and timed
out. After copying the four blobs to `/lib/firmware/`:

```
a640_zap: loading from 0x00000000f8a00000 to 0x00000000f8a01000
a640_zap: Brought out of reset
pil_tz: fw_load SUCCEEDED for soc:qcom,kgsl-hyp: rc=0
pil-a640_zap: active_count(1), active_time(3862), total_time(3862)
```

The zap is loaded, out of reset, and running — and it was loaded **before** the
GPU was initialised (`runtime_status` was still `unsupported` at that moment).

## 4. RESULT: zap does NOT fix rendering — and the test was cut short

With the zap loaded and the GPU freshly initialised, the compute→SSBO test ran
and completed before the wedge:

```
T1 compute -> SSBO: nonzero bytes=0/256   ALL ZERO - shader->buffer write dead
```

So **loading the zap is not sufficient**. Shader-domain writes are still
silently dropped with the zap running.

Caveat, stated plainly: T2/T3 never completed in that run, so **the FBO
readback with zap loaded is still unmeasured**. Only the compute result is firm.

## 5. The suspend wedge is not preventable from userspace

This run exposed that my earlier `control=on` workaround was **insufficient**:

```
control=on   status=suspending   autosuspend_delay_ms=66
```

`control=on` did **not** stop the wedge. `msm_gpu_retire()` calls
`pm_runtime_put_autosuspend()` after **every** submit (`msm_gpu.c:564`), and the
reference can reach zero regardless of the control file; the delay still reads
66 ms. The tree's own comments at `msm_gpu.c:379-390` and `:637-650` flag
unbalanced `pm_runtime_get/put` pairs from earlier images.

Consequence: **any GPU test can wedge mid-run**, which is why T2/T3 were lost and
why the earlier "draw-triggered freeze" may simply be *this* — a wedged suspend
rather than a hang in the render path. That reframes the freeze: it is likely
the known suspend bug wearing a different hat, not a new failure.

## 6. Image 247: hold runtime PM, permanently

Because the suspend callback blocks and cannot be dodged from userspace, the
fix is to make it unreachable. `msm_gpu_init()` now takes one extra runtime PM
reference that is deliberately **never released** (module param `pm_hold`,
default **ON**, `0644` so it can be turned off at runtime):

```c
if (pm_hold) {
	pm_runtime_get_sync(&gpu->pdev->dev);   /* intentionally unbalanced */
	pr_info("PMHOLD247: holding permanent runtime PM reference on %s\n", name);
}
```

usage_count can then never reach zero, autosuspend can never fire, and the
broken suspend path becomes dead code. Cost: no GPU power gating — irrelevant
on a bring-up kernel, and vastly better than a machine that wedges mid-test.

This should make every subsequent measurement reliable, which is the whole point:
we currently cannot tell a real result from a test that died in the suspend.

## 7. Where the evidence stands

| hypothesis | verdict |
|---|---|
| GPU half-suspended (draws discarded) | **confirmed**, and now fixed at the source |
| Unclocked RB/CCU/UCHE | refuted (243/245 hwcg verified vs CAF) |
| Wrong hwcg register addresses | refuted (no `0xdeadbead`, 43/46 exact) |
| Broken read path | refuted (`glReadPixels` works, returns sound zero) |
| Render-vs-handoff | resolved: **render itself** is broken |
| **Secure mode / no zap** | **zap now loads and runs — and it is NOT sufficient** |
| Shader-domain writes dead | **confirmed**, survives zap |

So secure mode is demoted: it was a real gap (the zap genuinely was never
loaded, and that is now fixed), but it is not the cause of the silent
shader-domain failure. The failure is still exactly: CP domain perfect, shader
domain silent, zero faults — now *with the zap loaded and running*.

See `ZAP-REAL-MECHANISM.md` and `IMAGE-245-RESULTS.md`.