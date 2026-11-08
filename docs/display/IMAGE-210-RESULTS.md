# Image 210 results — guard proved the re-entry theory dead; the freeze is elsewhere

Date: 2026-11-11. Evidence: `captures/freeze210-last_kmsg.log`.

## The guard did its job, and what it told us

```
handoffs on this boot : 1
"REFUSING" lines      : 0
```

Exactly one trigger was issued and exactly one bring-up ran. **The
duplicate-trigger / re-init theory is now fully dead** — it was never the cause.
The guard never fired because there was nothing to refuse.

And bring-up **completed cleanly**:

```
[  64.695312] a6xx-load:1148  CPDUMP[auto-rest] ... ---- end ----
```

So the machine did not freeze at bring-up. **The system kept running to
t=491.5** — over eight minutes after the GPU came up — during ordinary
background work (tsens, ssusb, qup_uart, sec_debug_partition). The log then
simply stops. That is a hang, but a late one, and not inside `hw_init`.

This retires the "second bring-up wedges the machine" hypothesis and, with it,
the main motivation for image 210. The guard is harmless and worth keeping as
cheap insurance, but it did not fix anything.

## The real new finding: `auto-post` never ran

```
"auto-post" occurrences : 0
```

`load_gpu_thread()` is:

```c
load_gpu_real(dev);
...
mutex_lock(&dev->struct_mutex);
a6xx_cp_state_dump(gpu, "auto-rest");     <- ran, t=64.7
mutex_unlock(&dev->struct_mutex);

msleep(12000);

mutex_lock(&dev->struct_mutex);
a6xx_cp_state_dump(gpu, "auto-post");     <- NEVER ran, expected ~t=76.7
mutex_unlock(&dev->struct_mutex);
```

The `a6xx-load` thread stops emitting immediately after `auto-rest`. It is
almost certainly wedged taking `dev->struct_mutex` for `auto-post`, or inside
`a6xx_cp_state_dump()`'s register reads.

**That is our own diagnostic code, added in image 186, sitting in the critical
path twelve seconds after every bring-up.** It has been running on every boot
this whole time. This is the first time we have had evidence that it wedges.

## Recurring unrelated oops

Every boot now ends with the same RPMh BUG, on a different workqueue each time:

```
kernel BUG at drivers/soc/qcom/rpmh.c:209!
qcom_rpmh_mailbox HW IRQ 37 is PENDING at GIC
qcom_rpmh_mailbox Tasklet is not active
```

| boot | workqueue |
|---|---|
| 209 freeze A | `ufs_clk_gating_0 ufshcd_ungate_work` |
| 209 freeze B | (also present) |
| 210 | `soc:battery sec_bat_monitor_work` |

This is a pre-existing defect in this tree's RPMh driver, unrelated to the GPU.
It kills a workqueue thread each boot. The system has survived it every time
observed, so it is not the freeze — but it is noise we should stop confusing
with signal, and it is worth recording as its own item.

## Where we actually stand

Solid, repeatable facts:

- Bring-up is reliable: `BABEFACE`, 7/7 HFI ACKs, `hw_init ret=0`, many boots running
- Mesa submits reach the kernel; the ring is written and the GPU fetches it
  (`RPTR` chases `WPTR`)
- Nothing executes: `CP_SCRATCH_REG2 = 0`, `fence` stuck at `0/1`
- `memptrs_iova == 0`, confirmed on three boots, so `rbmemptr(ring, fence) == 0x4`
- Leading hypothesis: the a6xx port never emits upstream's submit prologue —
  `CP_SET_PROTECTED_MODE`=0, `PERFCTR_SRAM_INIT_CMD`, `CP_SMMU_TABLE_UPDATE`.
  The opcode (83) is defined in `adreno_pm4.xml.h` and used nowhere.

Still missing, and it is the crux: **we have never actually read the ring
contents.** `RINGCPU = 0` on every attempt. Three consecutive runs have been
derailed before reaching a submit — once by a duplicate trigger, twice by a hang
whose cause is now, on this evidence, not the GPU trigger at all.

## Recommendation for image 211

Stop fighting the harness and remove the things that are demonstrably in the way:

1. **Delete the `auto-rest` / `auto-post` dumps.** They are our diagnostics,
   they run 12 s after every bring-up, and the evidence now says the thread
   wedges on them. They have already given us `rb_iova`, `RB_CNTL` and
   `RB_RPTR_ADDR` from a much older run; they are not earning their keep.
2. Keep `RINGCPU` — it lives in `a6xx_submit()`, not in the bring-up path, and
   it is the one measurement that settles the question.
3. Sequence strictly: trigger once → confirm `BABEFACE` → **copy dmesg to the
   host** → run `fbprobe.py` → copy dmesg again. Never trigger and test in one
   step, and always bank the first capture before doing anything that can wedge.

If that still freezes, we will at least have the bring-up state banked on the
host and can tell whether the hang is before or after the submit.

## Honest note on pace

Several consecutive images have been spent on harness and freeze forensics
rather than on the GPU bug. The 210 guards are correct but addressed a
hypothesis that the evidence has now ruled out. The next image should be small
and aimed only at getting `RINGCPU` onto the host.
