# Images 71/72 — firmware deployed, GPU core visibility still blocked

## Firmware deployment (done, verified)

Extracted from the Samsung vendor image (`zip → tar.md5 → lz4 → simg2img → ext4 → debugfs`),
recorded in `artifacts/display/GPU-FIRMWARE.md`.

| file | size | sha256 (host == device) |
|---|---|---|
| `a630_sqe.fw` | 32,304 | `a0e1b583f620fabe32729ce367959d1960638663244d7d0cfc21b9a5215a018b` |
| `a640_gmu.bin` | 37,680 | `3ff0c02708bbe78641db887fa62f3a7f9337934d0c2ce0b961ef7c43172591d2` |

Placed (isolated, nothing else added) at:

    /lib/firmware/qcom/a630_sqe.fw     <- "new" location, tried first
    /lib/firmware/qcom/a640_gmu.bin
    /lib/firmware/a630_sqe.fw          <- legacy location
    /lib/firmware/a640_gmu.bin
    /root/a6xx-fw-orig/                <- untouched originals kept on device

Why `/lib/firmware` and not the cmdline path: the cmdline sets
`firmware_class.path=/vendor/firmware_mnt/image`, but **that directory does not
exist on the Debian rootfs** — that was the origin of every `-74`. `/lib/firmware`
is compiled into the kernel (verified via `strings vmlinux`).

Driver expectation (`adreno_request_fw`, ported from v4.19), tried in order:
`qcom/<name>` direct → `<name>` legacy direct → `qcom/<name>` helper.
`ADRENO_FW_SQE = 0`, `ADRENO_FW_GMU = 1`, and the loop returns on first
failure — which is why only `a630_sqe.fw` ever appeared in the logs.

## IRQ487 -EBUSY — leak confirmed real, fix applied

Traced, not assumed:

1. `msm_gpu_init()` claims the IRQ with `devm_request_irq()` → succeeds.
2. `adreno_load_fw()` fails → `devm_free_irq`-less `gpu->funcs->destroy(gpu)`.
3. devres only releases the IRQ when the GPU **platform device** is released,
   which `destroy()` does not do. **Real leak.**
4. `load_gpu()` retries `adreno_load_gpu()` on *every* open while
   `priv->gpu == NULL`, so every retry hit `-EBUSY` — `-16`.

Fix: `devm_free_irq(&pdev->dev, gpu->irq, gpu)` on the firmware-failure path.
`pdev` there is `priv->gpu_pdev`, the same device `a6xx_gpu_init` used, so the
call is correctly paired (verified by tracing both ends). Symmetric — it removes
the devres node *and* frees the IRQ. Note this branch is **not taken** when
firmware loads, i.e. the fix was inert in image 71 and cannot explain its bootloop.

## Image 71 — immediate GPU load, firmware present → bootloop

Bootlooped before WiFi, so zero log bytes captured. pstore empty.

Inferring, not claiming: image 70 (firmware absent) boots; image 71 (firmware
present, IRQ fix inert) bootloops. So loading the real blobs lets the GPU get
further into genuine bring-up, which then hangs. The `-74`/`-2` firmware errors
are presumably gone — but this is unverified, because we could not see the log.

## Image 72 — GPU core load deferred 300 s → boots, but the work NEVER runs

`load_gpu()` deferral confirmed engaged:

```
[   17.529915] a6xx: deferring GPU core load by 300000ms
```

But across **two** full boots and 127 polls, `a6xx: deferred GPU core load
starting now` appeared **zero** times. The device stayed alive to 325.55 s,
well past the 317.53 s deadline, and the ring buffer provably held the whole
boot (12.06 s → 337.46 s), so it is not a log-overflow artefact.

Ruled out:
* **System suspend** — none. `systemd-inhibit` engaged at 100.05 s; only WiFi
  PCIe runtime PM (`BEFORE_EP_SUSPEND`/`AFTER_EP_RESUME`) appears, never a
  system suspend entry.
* **Log overflow** — ruled out above.
* **Missing symbol** — the sibling GMU deferral uses the identical pattern and
  *does* fire (`a6xx: deferred GMU probe starting now` at 340.96 s in image 70).

So the `delayed_work` is queued but never executes, and no evidence gathered so
far explains it. Suspect but **unconfirmed**: Samsung's screen-off freeze holds
`system_wq` frozen, and nobody is touching the phone. The GMU deferral in image 70
fired while the screen was on and we were actively poking the device.

## Capture-methodology notes

* `dmesg -w` over SSH **stalls** (stopped at 89 s while the device ran to 337 s).
  Replaced by host-side polling that dumps the full ring buffer every 2 s — each
  poll is self-contained, so the last successful poll always holds the newest
  state.
* Device-side files are useless: `/tmp` is a tmpfs overlay, `/data` is reset on
  reboot. Everything must land on the host.
* The `ping`-based "is it up" loop is unreliable here; SSH itself is the
  reliable liveness probe.

## Still blocked

No visibility into the GPU core init path. `GPU_ID` not attempted, per instructions.