# The ZAP experiment, completed — clean negative

Date: 2026-10-20
Device: image 276 (KGSL HFI/LOG layout) + the **stock** `a630_sqe.fw` restored
(md5 `42b0a156…`, 32304 B).

## What ran

Image 246 built the on-demand ZAP load and measured **only T1** before being cut
short. Its own comment is the reason this matters:

> "Without the zap the shader units silently do nothing while the CP domain
>  works."

This run loads the ZAP **before** the GPU is touched (as 246 requires), then
measures **all three** paths.

## The ZAP loaded cleanly

```
pil_tz: fw_load requested for soc:qcom,kgsl-hyp (pas_id 13, firmware 'a640_zap')
subsys-pil-tz soc:qcom,kgsl-hyp: a640_zap: loading from 0x00000000f8a00000 to 0x00000000f8a01000
subsys-pil-tz soc:qcom,kgsl-hyp: a640_zap: Brought out of reset
pil_tz: fw_load SUCCEEDED for soc:qcom,kgsl-hyp: rc=0
pil-a640_zap: active_count(1), active_time(801), total_time(801)
```

## Result — all three paths dead, with the ZAP running

| test | result |
|---|---|
| T1 compute → SSBO | prefill **256/256** intact, **0/64** matches — GPU WROTE NOTHING |
| T2 vertex → transform feedback | prefill **1024/1024** intact, **0/64** — GPU WROTE NOTHING |
| T3 FBO pixel readback | `after-clear=[0,0,0,0] after-draw=[0,0,0,0]` — **0/6 checks passed** |

**The secure firmware is loaded, out of reset, and running — and the shader still
writes nothing.** This closes the ZAP lead, and closes the gap image 246 left
open (T2/T3 were never measured with the ZAP loaded).

## Two operational fixes made along the way

1. **Stock `a630_sqe.fw` restored** (md5 `42b0a156…`), so the device now
   matches the KGSL reference exactly. The newer upstream blob is preserved on
   the host at `artifacts/fw/a630_sqe.fw.linuxfirmware-master` (md5 `9f2540d7…`).
2. **The phone's `/` was 100 % full** (0 bytes free) — the backup copy of the
   firmware actually *failed* with ENOSPC because of it. Moved the project's old
   capture logs to `/userdata/oldlogs`; `/` is now 51 % used (964 MB free).
   **Worth doing for any future run**: the ZAP blobs live in `/lib/firmware/`,
   and a full `/` can break firmware loading and sysfs writes.
