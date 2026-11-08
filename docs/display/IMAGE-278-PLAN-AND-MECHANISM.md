# Image 278 — the GPU SMMU aperture is never programmed in the secure world

Status: kernel patched and building. Nothing flashed yet.

## What image 277 actually established (and what it did not)

Image 277 asked the GPU's own CP to leave secure mode, the way stock KGSL
and mainline a5xx/a6xx do: `CP_SET_SECURE_MODE(0)` through the ring, not a
host register write.

**First run** — the ZAP never loaded, because the Android data clear had
taken `/lib/firmware/a640_zap.*` with it:

```
firmware a640_zap.mdt: _request_firmware_load: firmware state wait timeout: rc = -2
subsys-pil-tz soc:qcom,kgsl-hyp: a640_zap: Failed to locate a640_zap.mdt(rc:-11)
```

So the run was **inconclusive by construction**: without the ZAP the CP is
not authorised to write `SECVID_TRUST_CNTL`, which is exactly the condition
KGSL's own comment describes ("If successful we can use the CP to switch out
of secure mode").

**Second run** — the ZAP loaded cleanly (`fw_load SUCCEEDED`, `a640_zap:
Brought out of reset`, `pil-a640_zap: active_count(1)`), and the packet was
issued by the kernel's own `OUT_PKT7` (correct PM4 parity). Result:

```
SECVID277: CP path: before TRUST_CNTL=0x1 TSB_CNTL=0x0 TSB_BASE=0x0 SIZE=0x0
SECVID277: CP path: after  TRUST_CNTL=0x1 TSB_CNTL=0x0 TSB_BASE=0x0 SIZE=0x0
SECVID277: RPTR=0xb WPTR=0xb STATUS=0x1
```

The CP consumed the packet (RPTR 0x9 → 0xb, idle afterwards) and the gate
**did not move**. So `CP_SET_SECURE_MODE(0)` is **not** what flips
`TRUST_CNTL` on this part — or the CP's write to it is discarded. The
"secure mode" theory is therefore **closed negative** as the cause of the
dead write path, and the project should stop treating `TRUST_CNTL=0x1` as
the smoking gun it was assumed to be.

## A harness bug that invalidated two measurements

The userspace `CP_MEM_WRITE` discriminator (`/home/usb/.opencode/secvid_cp.py`)
was emitting type-7 headers **without the PM4 parity bits** this kernel sets
in `OUT_PKT7()`:

| packet | harness used | correct |
|---|---|---|
| `CP_SET_SECURE_MODE` | `0x70010066` | `0x70E60001` |
| `CP_MEM_WRITE` | `0x7003003D` | `0x703D8003` |

The formula reproduces a header this project has already seen in a live ring
(`CP_DRAW` = `0x70388003`), so it is the right encoding. The consequences
match the symptoms exactly: the write was silently discarded, and the second
submit left the CP desynced (`CP_RB_RPTR=0xb4` against `CP_RB_WPTR=0x14`,
`cp_busy=1` afterwards), which is what hung the following workload.

The harness now computes the headers with the parity rule and **asserts**
them against the known-good `CP_DRAW` value, and `run278.sh` refuses to run a
stale copy. Consequence: **the plain `CP_MEM_WRITE` result is still
unmeasured** — it is the first thing image 278 re-runs, correctly.

## The lead: a secure-world handshake the working driver makes and ours does not

Stock CAF KGSL programs the GPU's SMMU aperture **in TZ** on every GPU start:

```c
/* drivers/gpu/msm/adreno.c:1896 adreno_program_smmu_aperture() */
/* drivers/gpu/msm/kgsl_iommu.c:1298 */
desc.args[0] = 0xFFFF0000 | ((CP_APERTURE_REG & 0xff) << 8) |
                (context_bank_number & 0xff);
desc.args[1] = desc.args[2] = desc.args[3] = 0xFFFFFFFF;
scm_call2(SCM_SIP_FNID(SCM_SVC_MP, CP_SMMU_APERTURE_ID), &desc);   /* 0x0C / 0x1B */
```

with `context_bank_number` taken from the user context's
`DOMAIN_ATTR_CONTEXT_BANK` (`kgsl_iommu.c:1343-1351`). Upstream msm does the
same for a6xx (`qcom_scm_set_gpu_smmu_aperture(0)`, from `adreno_hw_init`).

**The msm/drm path in this tree never makes that call.** TZ is therefore never
told which SMMU aperture/context bank the GPU may use. On permissive firmware
that is invisible; on Samsung's TZ it can mean the GPU's user-memory traffic
is refused while the boot-time windows keep working.

That is the observed signature, precisely:

* the CP executes command streams and the memstore fence retires — but the
  fence is written by the **writeback** block to a fixed boot-time address,
  not by the CP's SMMU path;
* everything the GPU writes to a **user buffer** never lands, in every
  driver, for every buffer: 0/64 SSBO, 0/6 FBO, 0/4096 CP_BLIT;
* the SMMU reports faults for GPU traffic in a context bank the DT's
  `gfx3d_user` does not use (`cb=1`, `SID=0x405`, while `gfx3d_user` is
  CB 0 / SID 0x401);
* 167 of 213 archived captures contain an EL2/HYP `TZBSP_ERR_FATAL_NOC_ERROR`
  — a denied aperture would produce exactly those.

This is also the shape of the hypothesis the project started with: a missing
token handed to the secure world, after which the bus is switched into
isolation. The difference is that **the missing token is ours to provide** —
it is a kernel-side SCM call, not something Samsung must be asked for.

## The change (image 278)

`drivers/gpu/drm/msm/msm_gpu_mmu.c`, immediately after the GPU domain is
attached to `gfx3d_user`, gated on a new parameter `a6xx.smmu_aperture_cb`:

* `-2` — never call SCM (default, today's behaviour)
* `-1` — call it with the domain's `DOMAIN_ATTR_CONTEXT_BANK` (CAF's
  behaviour) — **this is what image 278 uses**
* `>= 0` — call it with that context bank number, to A/B against what the
  hardware actually uses

The log line carries everything needed to judge it:

```
SECVID278: aperture SCM: cb=<n> forced=-1 avail=<0|1> ret=<rc> (arg0=0x<hex>)
```

`ret` is TZ's answer: `0` means the secure world accepted the aperture.

## Falsification

* `ret=0` **and** the writes start landing → the aperture was the gate; the
  fix is a three-line SCM call, and the whole project turns on it.
* `ret=0` **and** the writes still do not land → TZ accepted the aperture but
  something else gates the data path; the next candidate is the context bank
  itself (`a6xx.smmu_aperture_cb=0` vs `=1`) and then the SMR/SID routing
  (`SID=0x405` vs the DT's `0x401`).
* `ret<0` or `avail=0` → this firmware does not implement the call; that
  itself is a result, and it moves the search to the hypervisor/XPU side
  (the EL2 `NOC_ERROR` aborts).
* `CP_MEM_WRITE` lands but RB/SP still do not → the CP's path is open and the
  gate is specific to the 3D domain (UCHE/GBIF), not to memory access in
  general.
