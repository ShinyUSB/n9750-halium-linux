# Zap: the real mechanism, found in Samsung's own device tree

This supersedes the implementation plan in `GPU-ZAP-FIRMWARE.md`. **No device-tree
change is needed. No driver port is needed. No new kernel image is needed.**

## What the old plan assumed, and why it was wrong

`GPU-ZAP-FIRMWARE.md` said implementing zap requires:

- a boot-critical `zap-shader` **child node** under `msm_gpu`
- a new **`reserved-memory` region** under `msm_gpu`
- a driver port of CAF's `a5xx_gpu.c:29-95` loader to a6xx, including a
  `qcom_pas_is_available()` check and a `firmware-name` read from the DT node

All of that is unnecessary. It was inferred, not read from Samsung's source of
truth.

## What Samsung actually does

Samsung's stock device tree, extracted from
`/home/usb/mnt-sda/stock/boot.img` (kernel appended DTB, 497,331 bytes; parsed
with `/tmp/opencode/fdt.py`, since `dtc` is not installed on this host):

```
/soc/qcom,kgsl-hyp {
        compatible         = "qcom,pil-tz-generic";
        phandle            = <0x5ef>;
        qcom,firmware-name = "a640_zap";
        qcom,pas-id        = <0xd>;      /* 13 */
};
```

That is the **entire** zap configuration. Note what is *absent*:

- **no `reg`** — the node does not describe a memory-mapped region
- **no `memory-region`**
- **no `zap-shader` node anywhere in the tree**
- no `reserved-memory` entry for the zap at all

The zap is not loaded by the GPU driver and needs no reserved memory. It is
loaded by the **generic PIL/TZ firmware loader**, keyed purely on
(`qcom,firmware-name`, `qcom,pas-id`).

## Our tree already has all of it

`arch/arm64/boot/dts/qcom/sm8150-gpu.dtsi:14-18`:

```dts
pil_gpu: qcom,kgsl-hyp {
        compatible = "qcom,pil-tz-generic";
        qcom,pas-id = <13>;
        qcom,firmware-name = "a640_zap";
};
```

- byte-for-byte equivalent to Samsung's node
- present in the **running** device tree at
  `/proc/device-tree/soc/qcom,kgsl-hyp`
- `CONFIG_MSM_PIL_SSR_GENERIC=y`, so `drivers/soc/qcom/subsys-pil-tz.c` is built
- and the node **is bound**: `/sys/bus/platform/drivers/subsys-pil-tz/soc:qcom,kgsl-hyp`
  is a live symlink to the platform device

So the plumbing was never the problem.

## The only thing that was actually missing: the firmware files

`drivers/soc/qcom/peripheral-loader.c:1301` (`pil_boot`):

```c
snprintf(fw_name, sizeof(fw_name), "%s.mdt", desc->fw_name);  /* "a640_zap.mdt" */
ret = request_firmware(&fw, fw_name, desc->dev);
if (ret)
        pil_err(desc, "Failed to locate %s(rc:%d)\n", fw_name, ret);
```

then `.b00`, `.b01`, ... segments. It looks for them in the standard firmware
path, i.e. `/lib/firmware/qcom/`.

On the device that directory contained only:

```
/lib/firmware/qcom/a630_sqe.fw
/lib/firmware/qcom/a640_gmu.bin
```

**No `a640_zap.*` at all.** So `request_firmware("a640_zap.mdt")` had nothing to
find, and the GPU was brought up with no zap — which is exactly the condition
under which shader units refuse to run while the CP domain works.

Pushed this session, sha256-verified against the host copies (which were
extracted from the Samsung-signed `apnhlos` image, not from vendor/modem):

| file | sha256 |
|---|---|
| `a640_zap.mdt` | `de3777f460f0dff729f93caf5a6dba5fb9f77a9fd9174ea4b782e29225876d52` |
| `a640_zap.b00` | `c9e39868fb6edcc30fc3574657f10dd1dfe22c9cd79e53de213a69703582091f` |
| `a640_zap.b01` | `79e020acb71453feecccafcee7b731f8b8ae02444e14cc8c2420c16d09ea058a` |
| `a640_zap.b02` | `e24a4f483b71ea19715910f01e1abf94280756df731cad05df48fd7714e34198` |

## Current status: zap is DEFINITIVELY NOT LOADED — and the fix is a code change

The negative-control test (rename `a640_zap.mdt` away, reboot) **resolved the
ambiguity, and it inverts the optimistic reading**:

| condition | GPU zap node | venus node |
|---|---|---|
| `.mdt` **present** | silent | (n/a) |
| `.mdt` **absent** | **still silent** | `Failed to locate venus.mdt(rc:-11)` |

If `pil_boot()` were being attempted for the GPU, removing the file would have
produced `Failed to locate a640_zap.mdt`. It produced nothing, while venus
logged its failure loudly in the same boot. Therefore:

> **`pil_boot()` is never called for the GPU's zap node. The a640 zap firmware
> is not loaded at all, and never has been.**

(`a640_zap.mdt` has been restored, sha256 `de3777f4…`.)

### Why nothing triggers it

`drivers/soc/qcom/subsys-pil-tz.c` — the probe **only registers** the subsystem:

```c
d->subsys_desc.powerup = subsys_powerup;      /* the only pil_boot caller */
...
d->subsys = subsys_register(&d->subsys_desc);  /* probe ends here */
```

and

```c
static int subsys_powerup(const struct subsys_desc *subsys)
{
        d->desc.fw_name = subsys->fw_name;
        ret = pil_boot(&d->desc);              /* firmware load happens here */
}
```

So the load only happens on a **powerup event**. Nothing on this system issues
one for the GPU: `drivers/gpu/drm/msm/` contains **no call to `pil_boot` or
`subsys_powerup` at all**, and no userspace trigger is exposed (`subsys`,
`subsys8` and `ramdump` on the device node are directories, not writable
attributes; `/dev/ramdump` is empty; no debugfs).

This also explains the original open question in `GPU-ZAP-FIRMWARE.md` about a
GPU-side loader. In **CAF kgsl** — the driver Samsung actually ships — the zap is
loaded by `a6xx_zap_load()` via `subsystem_get(a6xx_core->zap_name)`, which
looks up a DT node *by name* and reads its `qcom,firmware-name`. That "kgsl
subsystem" framework does not exist in this CAF **msm** tree. The node and the
firmware name are both present; only the loader is missing.

### What a fix therefore requires

One of:

1. **Trigger a powerup for the bound `soc:qcom,kgsl-hyp` subsystem** during GPU
   init, so the existing `subsys_powerup()` → `pil_boot()` path runs. Needs an
   exported way to reach `subsys_desc.powerup` (currently `static`), or a
   direct call into `pil_boot()` with a `pil_desc` initialised from that node.
2. **Port kgsl's `a6xx_zap_load()` / `subsystem_get()`** into this tree and call
   it from `a6xx_hw_init()`.

No DT change and no new firmware are needed either way.

**Risk to weigh:** both options issue an SCM secure-world firmware-load call for
PAS 13. Doing that wrongly can hard-hang the device, and this device *already*
intermittently freezes during ordinary GPU work. So this should be built with
the load behind a module parameter defaulting **OFF**, verified to boot cleanly
with it off, and only then enabled — not flipped straight on.

### Reminder: the freeze hazard

Running real GPU work on this device intermittently hard-freezes the phone
(observed twice: once on image 245 before the zap push, once after). The GPU is
no longer stuck in the half-suspended state, so draws now genuinely execute —
and something in that path can hang. Always launch detached with
`/root/poller.sh` running so data survives. See `CLEAN-SESSION-RECIPE.md`.