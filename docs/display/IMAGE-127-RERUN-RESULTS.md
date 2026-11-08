# IMAGE-127-RERUN-RESULTS.md (no rebuild, no reflash, no source change)

## Idle baseline (PART 1)
- Post-reboot, pre-trigger (~360s observed): no GPU/GMU probe, no HFI
  IRQ line, dmesg 82 lines total, capture log empty of markers.
  **Storm absent at idle.** (Probe is trigger-driven on this port.)

## Trigger + marker sequence (PARTS 3–4)
- Trigger t=388s. `BABEFACE ms~1`, `fw_start ret=0`.
- Full ACK chain: `1/4/3/11/14/10`, all error=0. `hfi_start ret=0`.
- `GPU_SET ok` → `SECVID ok` → `[07]` → `[08]` (STATUS=0) →
  `[09]` → `[10]` hwcg ok → `[11]` → **A640 GBIF path, VBIF skipped** →
  `GBIF[00..03]` readback=`0x71620` ✓ →
  `GBIF[04]` readback=**`0x0`** (wrote 0x3; no crash, recorded) →
  `[12]` → `CP_SQE_CNTL` ok → `[11]` cp_init →
  **`[12] after a6xx_cp_init ret=-22`** (1.0s idle timeout) →
  `[17]/[18]` irq masks → `[13]` → `[19]` → **`[20] adreno_hw_init ret=0`** →
  `[21]` ring → `[15]` → `[23]`/`[24]` OOB clear →
  **`[25] return ret=-22`**. hw_init RETURNED, device alive.
- First marker not reached: none missing through [25]; the failure is
  functional (`a6xx_idle` never true after `CP_ME_INIT` flush), not a hang.

## New frontier
First GPU command submission (`CP_ME_INIT` + flush) never idles
(`-EINVAL`). VBIF confirmed as the false path (GBIF writes land cleanly).
Next image territory: ring/CP idle path (RBBM wait-idle, interrupts, or
the missing address-mode enable from image 126).

## Storm onset (PART 5)
- No NACKs during bootstrap (all six ACKs clean at t=388.34s).
- Earliest retained `error=111` at t=701.5s (firmware seq 2702+).
  **Onset undetermined between t=389s and t=701s** (ring rotation).
  Classification by timing: C/D (after hfi_start / after hw_init) —
  NOT A (pre-bootstrap), NOT B (during bootstrap).
- Final count pre-reboot: 27.8M+ IRQs. Storm still ongoing at reboot.

## Post-run state (PARTS 6–7)
- No SMMU faults, no panic/Oops/BUG on record. Boot lived 700s+, then
  rebooted (cause unrecorded — silent, like prior hw_init-era reboots).
- Device alive on fresh boot (54s at readout). Capture service active,
  persistent log preserved (`/var/log/gmu127-capture.log` +
  `gmu127-capture-prereboot.log`).
- Quirk note: trigger-adjacent Wi-Fi flap observed again (SSH drop,
  recovered in ~5s pre-reboot).
