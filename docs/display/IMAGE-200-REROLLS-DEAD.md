# Image 200 rerolls — 0 for 4. The barrier theory is dead.

Date: 2026-10-10. Kernel `#145` (image 200, both TX barriers disassembly-verified
in the binary).

## Results

```
attempt 1 verdict: ret=-110 acks=0
attempt 2 verdict: ret=-110 acks=0
attempt 3 verdict: ret=-110 acks=0
attempt 4 verdict: ret=-110 acks=0
```

Method per attempt (`harness` inline as `/usr/local/bin/lucky.sh`): attach
`dmesg -w` into `/root/` **before** the trigger, fire one trigger, wait 70 s,
read `verify_fw_version ret=` and the `HFI-ACK` count from the capture. Each
attempt appends one line to `/root/lucky-tally.txt`, which survives resets.

## Conclusion

**The transmit-barrier theory is dead.** Both barriers are in the binary
(verified with `llvm-objdump --arch=aarch64`: two `dsb st` inside the send path,
payload→index and index→doorbell, in order), and the rate did not move: 0/4
here, 1-in-8+ counting image 195's single success.

CAF's comment ("make sure packet and write index are written before an
interrupt is raised") was suggestive, not conclusive, and I treated it as proof.
Image 200 should not have been built on that alone.

## What is now exhausted on the HFI path

Static programming verified with **zero divergence** from CAF:

| check | result |
|---|---|
| Queue IDs (cmd/msg/dbg/dsp 0/1/2/3, pri 10/10/40/20) | match |
| Queue header layout (12 u32 fields) | match |
| Table `size` units | dwords, match |
| `HFI_CTRL_INIT=1` + `CTRL_STATUS&1` poll | present, `STATUS=0x1` |
| `QTBL_ADDR` + `QTBL_INFO=1` | present |
| Pre-boot (`INITRES=0`, `BOOT_CONFIG=0x02`, fence, `SFR=chipid`, `CM3_CFG=0x4052`) | all present |
| TX barriers (payload→index→doorbell) | present, verified in binary |

## The one remaining named difference: the RX-side wait

We wait on IRQ completion:

```c
ret = wait_for_completion_timeout(&resp.complete, msecs_to_jiffies(5000));
```

signalled via IRQ → tasklet → `complete()`. CAF does not do this — it **polls**
`GMU2HOST_INTR_INFO & BIT0` directly (`a6xx_hfi_send_cmd_wait_inline`, 1000 ms).
Our mask unmasks MSGQ so IRQs *can* arrive (and did, once), but if the IRQ path
is lossy in any way, our 5 s wait dies where CAF's poll would survive.

Converting our wait to a poll loop is small, safe, and removes the last
unexamined difference on this path.

## Decision rule, stated in advance

- If a poll-based wait still times out with `INTR_INFO == 0x0`, the firmware
  genuinely never posts, and the problem is inside the firmware's view of
  memory. That means revisiting the queue mapping with fresh eyes — not more
  barriers, not more ordering tweaks.
- If the poll succeeds, the IRQ-completion wait was the bug, and the lesson is
  that an interrupt-driven wait on a path the reference implementation polls is
  itself a porting bug regardless of outcome.

## Retraction

Image 200's headline ("the missing barrier") is retracted as a fix. The barrier
was genuinely missing and is genuinely restored, but it changed nothing. A fix
that changes nothing is not a fix.