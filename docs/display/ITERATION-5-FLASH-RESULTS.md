# Iteration 5 — Flash result: root cause confirmed, new blocker found

**Flashed:** BOOT partition only, via `heimdall flash --BOOT` (Heimdall v2.2.2).
No PIT, no repartitioning, no userdata/EFE/recovery/bootloader write. Exit status 0.

---

## 1. The flash is verified on the device

Read back from `/dev/block/bootdevice/by-name/boot`:

| region | on-device sha256 | expected | result |
|---|---|---|---|
| header | `kernel_size=48519996 kernel_addr=32768 ramdisk_size=4106247 ramdisk_addr=33554432` | same | match |
| kernel payload (offset 4096, 48,519,996 B) | `123a29cf9b4ccf1d56a3dcb6ef1991c922a26f78b6a19b6b163aa87d387b9f4e` | `123a29cf…` | **byte-exact** |
| ramdisk payload (offset 48,525,312, 4,106,247 B) | `0bbac4577f3567aec935c958216de5d30c7355452ca56248d6728f4f2634bdb6` | `0bbac457…` | **byte-exact** |

Heimdall log: PIT downloaded, `BOOT upload successful`, session ended, device rebooted.
Rollback paths intact: stock `boot.img` (`6718aba7…`) and iteration 2's image (`a5e3d8b2…`).

*(My first two verification reads reported mismatches — those were my own `dd`
block-arithmetic errors, not a bad flash. Corrected offsets: kernel `skip=1`,
ramdisk `skip=11847`, both with `bs=4096`.)*

## 2. The diagnostic printk worked — the ABL command line is finally visible

This is the first time the effective `/chosen/bootargs` has ever been read on this
device. Saved to `artifacts/device/iter5-abl-bootargs.txt` (71 tokens, 1,972 bytes).

The decisive region, **in order**:

```
19: androidboot.verifiedbootstate=orange
20: androidboot.keymaster=1
22: androidboot.ulcnt=2
23: root=PARTUUID=97d7b011-54da-4835-b3c4-917ad6e73d74
...
55: androidboot.boot_recovery=0
56: skip_initramfs          <<<<<< HERE IT IS
57: rootwait
58: ro
59: init=/init
```

**`skip_initramfs` is confirmed, on the device, in ABL's own generated bootargs.**

This closes iteration 3's open question. It is a bare token immediately after
`androidboot.boot_recovery=0`, followed by `rootwait ro init=/init` — ABL's
system-as-root contract.

### Hypothesis verdict (from iteration 4)

| hypothesis | verdict | basis |
|---|---|---|
| B — AVB failure forces `skip_initramfs` | **disproved** | recovery boot has identical AVB state (`orange`, `ulcnt=2`, unlocked, `AUTHENTICATE fail`) and its fully-readable cmdline has **no** `skip_initramfs`; its `androidboot.boot_recovery=1` |
| C — some other boot-image property | **supported, and narrowed** | the discriminator visible in both bootargs is `androidboot.boot_recovery` = 0 vs 1 |
| A — unconditional for the normal-boot path | **effectively confirmed** | ABL appends `skip_initramfs rootwait ro init=/init` on the `boot_recovery=0` path only |

Also note `root=PARTUUID=97d7b011-…` (token 23) is present on **both** the
`boot_recovery=0` and `boot_recovery=1` paths — confirming iteration 4's finding
that `root=/system` is unconditional and **not** an AVB fallback.

## 3. The fix worked — the ramdisk is now unpacked and `/init` is executed

In all **4** bootloop cycles, identically:

```
[    0.653517]  Trying to unpack rootfs image as initramfs...   <-- ABSENT in iteration 2
[    0.705966]  Freeing initrd memory: 4008K
```

And the backtrace shows we now get all the way to **exec'ing our Halium `/init`**:

```
call_usermodehelper_exec_async
ret_from_fork
do_execve
do_execveat_common
search_binary_handler
security_bprm_check
five_bprm_check            <-- Samsung FIVE LSM
__five_bprm_check
push_file_event_bunch
queue_work_on
__queue_work               <-- FAULTS HERE
```

**Android no longer runs at all.** The only `vold`/`apexd` lines in the buffer are at
t≈4237 s, i.e. *before* the first new-kernel session (line 9139) — they belong to the
old iteration-2 boot still resident in the log. Zero Android in any of the 4 new sessions.

This is the first time in the project that our Halium initramfs has actually executed.

## 4. NEW BLOCKER: Samsung FIVE security framework panics on the first execve

```
[    1.503156]  Kernel panic - not syncing: Fatal exception
```

Identical in all 4 cycles, same faulting instruction every time:

```
Code: aa0103f6 13001d08 2a0003f5 36f82aa8 (39440ac8)
```

which is `mov x22,x1; sub x8,x0,#0x740; cmp x5,#0; tbz w8,#21,[x19]` — a list walk
whose head pointer is `0x39440ac8`. That is **not a valid kernel address**
(kernel pointers here are `0xffffff80…`), so `__queue_work` is dereferencing an
uninitialised/garbage workqueue `pool`.

Supporting observations:
- **Zero** `five*` messages anywhere in the 2 MB log — the FIVE framework emits no
  init output at all, consistent with it never initialising its workqueue.
- `security_bprm_check` fires on the **first** execve in the kernel's life. Our
  `run_init_process("/init")` happens at t≈1.47, versus iteration 2 where Android's
  init only started at t≈3.16 (after `VFS: Mounted root` at t=1.610).
- Panic handler: `sec_debug_panic_handler :Fatal exception` →
  `sec_debug_pm_restart` → `Going down for restart now` → reboot. That is why the
  device **bootlooped and did not auto-enter TWRP** — the restart is a PM reset, not
  Android's `reboot, recovery`. The user entered TWRP manually.

### Is this our fault?

**No — it is a pre-existing defect in Samsung's FIVE LSM that our change merely
exposes.** Nothing in the two source edits touches LSMs, workqueues, or the exec
path. The stock path never execs an initramfs `/init`, so it never reached this
code. Now that `skip_initramfs` is neutralised, the first exec of the boot is ours
and it trips the bug.

This is exactly the trade-off flagged in iteration 4: a failing `/init` now panics
instead of silently falling through to stock Android. Here `/init` did not even get
to run a single instruction — the kernel died in the LSM hook on the way to it.

## 5. Device state

- Device: `RF8M82H3NFM`, in **TWRP/recovery**, ADB online. Fully recoverable.
- BOOT: holds our iteration-5 image (verified above).
- Untouched: PIT, userdata, EFS, `system`, `recovery`, `dtbo`, bootloader.
- The device is not bricked and is not stuck — it boots, panics, and reboots.

## 6. Where this leaves the investigation

Iteration 2's question is **fully answered**: the ramdisk was fine, ABL put
`skip_initramfs` in its bootargs, and that alone caused the Android boot.

The port has advanced one stage: from "our initramfs is discarded" to "our
initramfs is unpacked and the kernel tries to run it, then Samsung's FIVE LSM
kills the process."

Open questions for the next step, none of which I have acted on:

1. **Why is FIVE's workqueue uninitialised?** Candidate leads worth checking in the
   source, all read-only: whether `five`'s workqueue is created by an initcall that
   does not run or fails; whether `push_file_event_bunch` is reached on a path that
   assumes a later boot stage; whether `CONFIG_SECURITY_FIVE`-equivalent options
   could be satisfied by the Halium profile fragment instead.
2. **Is the crash specific to executing a *script*?** Our `/init` is `#!/bin/sh`.
   A static ELF as `/init` would take a different `search_binary_handler` path.
   Cheap to test offline by unpacking the ramdisk and reading the script.
3. **Does the stock kernel survive the same exec?** Answerable only by booting the
   preserved stock `boot.img`, which is a stock-firmware restore, not a port step.

No further kernel edits, no further flashes, and no `BOARD_KERNEL_CMDLINE` change
have been made. Awaiting direction.

## Artifacts

* `artifacts/device/iter5-flash.log` — Heimdall session
* `artifacts/device/logs/iter5-last_kmsg-raw.bin` / `.txt` — 2 MB kernel log, 4 sessions
* `artifacts/device/iter5-abl-bootargs.txt` — the recovered ABL `/chosen/bootargs`
