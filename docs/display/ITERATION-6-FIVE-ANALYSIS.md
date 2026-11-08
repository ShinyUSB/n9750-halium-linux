# Iteration 6 — FIVE/LSM panic: source, binary and log analysis

**READ-ONLY.** No source written, no config changed, no build, no flash, no reboot, no
device partition touched. All findings come from the existing tree, the existing
iteration-5 `vmlinux`, and the captured `last_kmsg`.

---

## 0. Correction to my iteration-5 statement

In iteration 5 I wrote that the backtrace showed "our Halium `/init` being exec'd".
**That was wrong, and this iteration disproves it.**

This tree's `run_init_process()` calls `do_execve()` **directly in the calling
context** — it does *not* fork:

`init/main.c:1275-1281`
```c
static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	return do_execve(getname_kernel(init_filename),
			 (const char __user *const __user *)argv_init,
			 (const char __user *const __user *)envp_init);
}
```

The panic frame below it is `call_usermodehelper_exec_async` ← `ret_from_fork`, i.e.
the exec happened in a **forked kernel thread**, and `init/main.c` contains no
usermodehelper call at all (only `usermodehelper_enable()` at line 1252).

**Therefore the exec that faulted was NOT `/init`.** It was some
`call_usermodehelper()` invocation, and it occurred *while the late initcalls were
still running*. The Halium initramfs was unpacked, but its `/init` was never
exec'd.

---

## TASK 1 — the exact LSM exec path

### Dispatch (Samsung-modified generic code)

`fs/exec.c:1659` (inside `search_binary_handler()`)
```c
	retval = security_bprm_check(bprm);
```

`security/security.c:349-360`
```c
int security_bprm_check(struct linux_binprm *bprm)
{
	int ret;

	ret = call_int_hook(bprm_check_security, 0, bprm);   /* 353 */
	if (ret)
		return ret;

	ret = five_bprm_check(bprm);                        /* 356  <-- Samsung direct call */
	if (ret)
		return ret;

	return ima_bprm_check(bprm);                        /* 359 */
}
```

**Important:** `five_bprm_check()` is **not** in the `lsm_hook_defs` chain. It is
called *directly* from `security_bprm_check()`. So no LSM registration is required
for it to run, and it cannot be disabled through LSM ordering.

`struct linux_binprm` is declared in `include/linux/binfmts.h`; the members this
path uses are `bprm->file` and `bprm->recursion_depth`.

### Which FIVE variant is compiled

`out/.config`:
```
CONFIG_FIVE=y
CONFIG_PROCA=y
CONFIG_PROCA_S_OS=y        (CONFIG_FIVE_GKI_20 is NOT set)
```
`security/samsung/five/Makefile` selects `s_os/five_main.o` when
`CONFIG_PROCA_S_OS` is set and `CONFIG_FIVE_GKI_20` is not. Confirmed on disk:
`out/security/samsung/five/s_os/five_main.o` exists; `five/five_main.o` and
`gki/five_main.o` do **not**. Only the `s_os` variant defines both
`__five_bprm_check` *and* `five_bprm_check` — matching the backtrace.

In `vmlinux`: `__five_bprm_check` @ `0xffffff80084d6f80`, `five_bprm_check` @
`0xffffff80084d7050`.

### The exact call chain, with file:line

```
do_execve()                            fs/exec.c
  do_execveat_common()                 fs/exec.c
    search_binary_handler()            fs/exec.c:1659  security_bprm_check(bprm)
      security_bprm_check()            security/security.c:349
        five_bprm_check()              security/security.c:356
          security/samsung/five/s_os/five_main.c:672-675
            __five_bprm_check()        security/samsung/five/s_os/five_main.c:639
              push_file_event_bunch() security/samsung/five/s_os/five_main.c:649
                queue_work(g_five_workqueue, &context->data_work)
                                     security/samsung/five/s_os/five_main.c:333
                  queue_work_on()      kernel/workqueue.c
                    __queue_work()    kernel/workqueue.c:1397
                      *** FAULT ***
```

Panic frames (kernel's own symbolisation, identical in all 4 boot cycles):
```
__queue_work+0x38/0x648
queue_work_on+0x50/0x70
push_file_event_bunch+0x264/0x298
__five_bprm_check+0x64/0xd0
five_bprm_check+0x14/0x24
security_bprm_check+0x60/0x78
search_binary_handler+0x38/0x1e0
do_execveat_common+0x618/0x82c
do_execve+0x2c/0x3c
call_usermodehelper_exec_async+0xdc/0x15c
ret_from_fork+0x10/0x1c
```

`push_file_event_bunch` is declared at `s_os/five_main.c:300` and its reported size
is `0x298` (664 B), so frame `+0x264` lies inside lines 300-350 — the only
`queue_work` in that function is **line 333**.

### Guard that does *not* protect this path

`s_os/five_main.c:307-308`
```c
	if (unlikely(!is_five_enabled()) || five_check_params(task, file))
		return 0;
```
* `is_five_enabled()` — `s_os/five_main.c:162-165` returns **`1` unconditionally** in
  the `S_OS` build.
* `five_check_params()` — `s_os/five_main.c:235-247` returns 1 (skip) only if
  `!file` or `!S_ISREG(inode->i_mode)`, i.e. **only for non-regular files**.

---

## TASK 2 — the crashing instruction

### The parenthesised word is code, not an address

`39440ac8` decodes as:

| field | value | meaning |
|---|---|---|
| size (31:30) | `00` | byte access |
| (29:27) | `111` | load/store immediate |
| V (26) | `0` | 64-bit base register |
| opc (25:24) | `01` | LDRB |
| (23:22) | `01` | unsigned offset |
| imm12 (20:10) | **`0x102` = 258** | byte offset |
| Rn (9:5) | `22` | `x22` |
| Rt (4:0) | `8` | `w8` |

**=> `ldrb w8, [x22, #0x102]`**

This is confirmed three independent ways:
1. The panic's own fault line: `[0000000000000102] user address but active_mm is swapper`
   and `Unable to handle kernel NULL pointer dereference at virtual address 00000102`.
2. `imm12` (`0x102`) equals the reported fault address exactly.
3. The raw word at `__queue_work+0x38` in the iteration-5 `vmlinux` is `0x39440ac8`.

### Register provenance

Raw words in `__queue_work` (link `0xffffff80080d0108`, iteration-5 `vmlinux`):
```
+0x20  d53b4228   msr daifset, #2
+0x24  aa0203f3   mov x19, x2
+0x28  aa0103f6   mov x22, x1
+0x2c  13001d08   sub  x8, x0, #0x740
+0x30  2a0003f5   cmp  x5, #0
+0x34  36f82aa8   tbz  w8, #21, [x19]
+0x38  39440ac8   ldrb w8, [x22, #0x102]     <-- faulting PC
```
These match the panic's code dump word-for-word.

`kernel/workqueue.c:1397-1398`
```c
static void __queue_work(int cpu, struct workqueue_struct *wq,
			 struct work_struct *work)
```
so `x1 = wq`, `x2 = work`. From the panic register dump:
```
x22 = 0x0000000000000000      <- wq
x19 = 0xffffffc081fe0700      <- work (slab address, plausible)
```

### What is at `wq + 0x102`

From the DWARF of the same `vmlinux` (`pahole -C workqueue_struct`):
```
unsigned int flags __attribute__((__aligned__(128))); /*   256     4 */
```
`struct workqueue_struct.flags` is at **offset 256 (0x100), 4 bytes**, so it occupies
`0x100-0x103`. Offset `0x102` is **byte 2 of `wq->flags`**.

### Verdict on `0x39440ac8` / `0x102`

* It is **the encoding of the faulting instruction**, not a data value, pointer,
  list head, offset, or statically-allocated object. (In arm64 oops output the
  parenthesised word after `Code:` is the instruction at the faulting PC.)
* `0x102` is the **effective address**, produced by adding `imm12` to a **NULL**
  base register.

**=> The workqueue pointer passed to `__queue_work()` was `NULL`.** This is proven
at register level, not inferred.

Since the only `queue_work` on this frame is `s_os/five_main.c:333`, the pointer
was `g_five_workqueue`, and:

```c
s_os/five_main.c:49    static struct workqueue_struct *g_five_workqueue;   /* BSS => NULL */
s_os/five_main.c:333       rc = queue_work(g_five_workqueue, &context->data_work) ? 0 : 1;
s_os/five_main.c:718   int __init init_five(void)
s_os/five_main.c:722       g_five_workqueue = alloc_workqueue("%s", WQ_FREEZABLE | WQ_MEM_RECLAIM, 0, "five_wq");
s_os/five_main.c:724       if (!g_five_workqueue)
s_os/five_main.c:960   late_initcall(init_five);
```

`g_five_workqueue` is written in exactly one place. It was still its initial
(BSS) value `NULL` at the time of the call.

Note: I have **not** disassembled `push_file_event_bunch` to read the register
actually passed in `x0` at the `queue_work` call site. The identification of
`g_five_workqueue` as the NULL pointer rests on it being the sole `queue_work` in
the 664-byte function at frame `+0x264` — strong, but a disassembler check would
make it absolute.

---

## TASK 3 — FIVE initialisation

### Global state FIVE requires

| item | location | notes |
|---|---|---|
| `g_five_workqueue` | `s_os/five_main.c:49` | `static`, BSS, NULL until `init_five()` |
| `five_enabled` | `s_os/five_main.c:122-125` / `162-165` | two definitions under `#ifdef`; in the `S_OS` build `is_five_enabled()` returns `1` literally |
| `memfd_file` | set in `init_five()` (`s_os/five_main.c:741`) | after the workqueue |
| `five_hash_algo` | `s_os/five_main.c:702` (`__ro_after_init`) | set by `hash_setup()` inside `init_five()` |

### The single initialisation site

`s_os/five_main.c:718-760`, in order:
1. `alloc_workqueue(...)` → `g_five_workqueue`   (line 722)
2. `hash_setup(CONFIG_FIVE_DEFAULT_HASH)`       (line 729)
3. `five_init()`                                (line 733)
4. `five_hook_wq_init()`                        (line 738)
5. `memfd_file = shmem_kernel_file_setup(...)`  (line ~747)
6. `init_fs()`                                  (line ~755)
7. `five_dsms_init("1", 0)`
8. `five_init_dmverity()`

### Initcall level and registration

`s_os/five_main.c:960` — `late_initcall(init_five)`, i.e. **initcall level 7**, the
last level. There are **no other call sites** for `init_five` in this tree (the
only other definitions are the unused `five/` and `gki/` variants).

It **is** linked into the built kernel: `nm` shows
```
ffffff800a503138 d __initcall_init_five7
ffffff800a503078 D __initcall7_start
ffffff800a438774 T init_five
```
`__initcall_init_five7` sits inside the level-7 initcall table, so
`do_initcalls()` **will** invoke it.

### Conditions / dependencies

* No `cmdline`, DT, filesystem, Android-property or boot-mode dependency in the
  workqueue creation itself — `alloc_workqueue()` is unconditional and first.
* No evidence anywhere in the tree that FIVE is expected to be initialised by
  Android userspace. `push_file_event_bunch` is a kernel-side LSM hook.
* The `S_OS` build disables the `five_enabled` runtime switch (`is_five_enabled()`
  returns `1`), so there is no runtime flag that could have kept the hook dormant.

### Timeline, from the log

```
[1.474705]  [4: swapper/0: 1] registered taskstats version 1
[1.474710]  [4: swapper/0: 1] Loading compiled-in X.509 certificates
[1.475183]  [3: kworker/u16:11: 408] Unable to handle kernel NULL pointer
```

`Loading compiled-in X.509 certificates` is emitted by the late built-in
certificate loader, i.e. **level-7 initcall output from PID 1, 0.47 ms before the
fault**. The faulting thread is a *different* thread (`kworker/u16:11`, pid 408)
that was forked by `kernel_thread()`.

So the usermodehelper exec ran **concurrently with the tail of `do_initcalls()`**,
while `init_five()` — itself in the same level-7 list — had not yet executed.

Reference ordering in `init/main.c` `kernel_init_freeable()`:
`do_basic_setup()` → `console_on_rootfs()` → `/init` accessibility check →
`prepare_namespace()` (only if `/init` is absent) → `run_init_process()`.
`do_basic_setup()` is where `do_initcalls()` lives.

### Why the exec was not `/init` — the candidate callers

`call_usermodehelper_exec()` is called from:

| site | what it execs |
|---|---|
| `security/keys/request_key.c:86` | `/sbin/request-key` |
| `lib/kobject_uevent.c:529` | `/sbin/ueventhelper` |
| `security/dsms/dsms_kernel_api.c:161` | dsms helper |
| `kernel/kmod.c:99` | `modprobe` (module autoload) |
| `fs/coredump.c:652` | `system-coredump` |
| `init/do_mounts_initrd.c:79` | `ramdisk` helper |

The log contains **no** `Direct firmware load`, `modprobe` or `request_key` messages.
Given the density of device probes immediately before the fault, `/sbin/ueventhelper`
(`lib/kobject_uevent.c:529`) is the most plausible, but that is **not established**.

---

## TASK 4 — why stock Android survives

The FIVE hook is invoked **unconditionally on every `execve`** — it is called
directly from `security_bprm_check()` (`security/security.c:356`), outside the
`lsm_hook_defs` chain, and `five_check_params()` only skips **non-regular** files.
Stock Android's `/system/bin/init` is a regular file, so stock Android's init
**does** reach exactly the same FIVE hook and the same `queue_work()` call.

The difference is therefore **not** the file, the ELF/script shape, or the LSM
path. It is **when** the exec happens relative to `late_initcall(init_five)`.

Evidence that stock Android gets there later:
* Iteration 2: `VFS: Mounted root` at t=1.610, first Android service at t=3.163.
* Iteration 5: the usermodehelper exec faults at t=1.475, while level-7 initcalls
  are still running.

**Why an early usermodehelper exec happens now and did not in iteration 2 is NOT
ESTABLISHED.** It is a genuine difference that this read-only iteration cannot
explain from the available logs; the log contains no message identifying which
usermodehelper helper was being exec'd.

---

## TASK 5 — `/init` shape (secondary hypothesis)

Extracted read-only from the flashed image
`artifacts/flash/05-halium-boot-skipinitramfs-noop.img` (ramdisk at file offset
48,525,312, 4,106,247 B, gzip):

| property | Halium `/init` |
|---|---|
| size | 5,960 B |
| type | `POSIX shell script, ASCII text executable` — **not ELF** |
| shebang | `#!/bin/sh` (bytes `23 21 2f 62 69 6e 2f 73 68 0a`) |
| mode | `0755` (`-rwxr-xr-x`) |
| interpreter | `/bin/sh` (present in the same initramfs under `bin/`) |
| cpio xattrs / SELinux metadata | none present in the archive (plain `newc` cpio, no xattr records) |

Comparison with the stock Android init was **not** performed: the stock
`/system/bin/init` cannot be read without mounting the 6.2 GB `/system`
partition, which is out of scope for this read-only iteration.

**binfmt consequence:** for a shebang script, `search_binary_handler()` invokes
`security_bprm_check()` **twice** — once with `bprm->file` = the script, then again
after re-exec with `bprm->file` = `/bin/sh`. Both are regular files, so
`five_check_params()` returns 0 both times and both would reach `queue_work()`.

**This makes the script shape a poor candidate for the trigger**, because a
regular-file ELF would behave identically with respect to `five_check_params()`.
It is retained only as a difference in *how many times* the hook fires.

---

## TASK 6 — facts vs hypotheses

### PROVEN

1. The complete call chain from `do_execve()` to the fault, with exact file:line
   (Task 1). `five_bprm_check()` is called directly from `security/security.c:356`,
   outside the `lsm_hook_defs` chain.
2. The compiled FIVE variant is `security/samsung/five/s_os/` (`CONFIG_PROCA_S_OS=y`;
   only `s_os/five_main.o` was compiled; only `s_os` defines `__five_bprm_check`).
3. The faulting instruction is `ldrb w8, [x22, #0x102]`, byte 2 of
   `struct workqueue_struct.flags` (DWARF: `flags` at offset 256, 4 bytes).
4. `x22` is loaded from `x1` at `__queue_work+0x28` and is `0x0` at the fault.
   `__queue_work(int cpu, wq, work)` ⇒ **`x1 = wq`**.
5. **The workqueue pointer passed into `__queue_work()` was NULL.** Confirmed by the
   kernel's own "NULL pointer dereference at virtual address 00000102" message and
   by ESR `0x96000005` (DABT, lower EL, translation fault L0, read, `active_mm is
   swapper`).
6. `g_five_workqueue` is a BSS `static` assigned in exactly one place,
   `s_os/five_main.c:722`, inside `init_five()`, registered `late_initcall` at line 960.
7. `is_five_enabled()` returns `1` unconditionally in this build; `five_check_params()`
   skips only **non-regular** files.
8. `init_five` is present in the level-7 initcall table (`__initcall_init_five7`).
9. The panic is byte-identical across all 4 boot cycles, including the faulting word.
10. **The faulting exec was NOT `/init`.** `run_init_process()` in this tree calls
    `do_execve()` directly; the exec came through `call_usermodehelper_exec_async`
    ← `ret_from_fork`, and `init/main.c` contains no usermodehelper call.
11. At the moment of the fault (t=1.475183), PID 1 was still emitting level-7
    initcall output (`Loading compiled-in X.509 certificates`, t=1.474710).
12. The Halium initramfs **was** unpacked in all 4 cycles
    (`Trying to unpack rootfs image as initramfs...`), and no Android userspace ran.

### STRONGLY SUPPORTED (not directly proven)

* The NULL pointer is specifically **`g_five_workqueue`**, rather than some other
  workqueue. Basis: it is the only `queue_work()` inside the 664-byte
  `push_file_event_bunch` that frame `+0x264` lies in.
* `init_five()` had not yet run when the usermodehelper exec faulted. Basis: the
  fault is concurrent with level-7 initcall output from PID 1, and `init_five` is a
  level-7 initcall.
* Stock Android's init also passes through this same FIVE hook and was protected
  only by executing later. Basis: the hook is unconditional for regular files, and
  iteration 2's Android init ran at t≈1.6-3.2 s.
* The exec'd helper is most likely `/sbin/ueventhelper`. Basis: `lib/kobject_uevent.c:529`
  is the only unconditional usermodehelper site consistent with heavy device probing
  at t≈1.47 with no firmware/modprobe messages.

### NOT YET ESTABLISHED

* **Why** `g_five_workqueue` was still NULL: whether `init_five()` had not been
  reached, or was reached and `alloc_workqueue()` failed. The log contains zero
  `five*` messages, so neither can be distinguished. (An `alloc_workqueue()` failure
  at t≈1.4 s with 12 GB RAM is implausible, but not disproven.)
* **Which** usermodehelper helper was being exec'd, and **what** in this boot
  triggers a usermodehelper exec during initcalls that iteration 2 did not have.
* Whether the exec'd helper is a regular file — inferred from the fact that the
  fault occurred past the `five_check_params()` guard, but the file's mode was not
  observed.
* Whether stock Android's `/system/bin/init` reaches the hook with the same
  argument values. Not read; `/system` was not mounted.
* Any role for `bprm->recursion_depth` (used at `s_os/five_main.c:674`); it affects
  *which* `five_bprm_check` variant is entered but not whether the workqueue is
  touched.

**Explicitly NOT claimed:** that FIVE is "uninitialised" in any general sense; that
the workqueue object is uninitialised (the pointer is NULL — there is no object at
all); that `/init` being a shell script is the trigger; that the `unused`/`os_version`
header fields are involved.

---

## Minimum causally correct change — HYPOTHESIS ONLY (not implemented)

The crash is a NULL-pointer dereference of `g_five_workqueue` inside
`push_file_event_bunch()` on a usermodehelper exec that precedes
`late_initcall(init_five)`. The **causally minimal** change would be to make
`push_file_event_bunch()` tolerate an unallocated workqueue, e.g. at
`s_os/five_main.c:332-333`:

```c
		INIT_WORK(&context->data_work, work_handler);
		rc = queue_work(g_five_workqueue, &context->data_work) ? 0 : 1;
```
→
```c
		INIT_WORK(&context->data_work, work_handler);
		if (g_five_workqueue)
			rc = queue_work(g_five_workqueue, &context->data_work) ? 0 : 1;
		else
			kfree(context);        /* or drop the event */
```

**Status: hypothesis, not proven.** It is the smallest change that removes the exact
fault, and it matches the guard style already used three lines above
(`if (unlikely(!context)) return -ENOMEM;`). But it has two unresolved costs that
must be settled before it could be recommended:

1. It silently drops FIVE's integrity event for that exec. Whether FIVE's security
   model tolerates an unmeasured helper is unknown.
2. It does not address the underlying ordering hazard, so any *other* early usermodehelper
   exec path into FIVE that dereferences other not-yet-created FIVE state
   (`memfd_file`, `five_init()` state) could fault similarly.

A second, more conservative candidate is to promote `init_five` to an earlier
initcall level, but that changes FIVE's own initialisation ordering and is
*larger* and less clearly correct than the guard.

**No change is recommended for action yet.** The next decisive read-only step would
be to identify the exec'd helper — which needs either a `dump_stack()` in the FIVE
hook or a capture of the `/proc/last_kmsg` `ueventhelper`/`request-key` evidence —
because if the helper is in fact something Samsung's boot depends on, the guard
above could mask a real problem.

---

## Files cited

* `fs/exec.c:1659`
* `security/security.c:349-360`
* `security/samsung/five/s_os/five_main.c:49, 122-125, 162-165, 235-247, 300-350, 333, 639-676, 718-760, 960`
* `security/samsung/five/Makefile` (variant selection)
* `kernel/umh.c:63, 117-123, 169-183` (Samsung usermodehelper)
* `kernel/workqueue.c:246-286` (`struct workqueue_struct`), `1397-1404` (`__queue_work`), `queue_work_on`
* `include/linux/workqueue.h`, `include/linux/binfmts.h`
* `init/main.c:1252, 1275-1281, 1374, 1404-1407`
* Callers of `call_usermodehelper_exec()`: `security/keys/request_key.c:86`,
  `lib/kobject_uevent.c:529`, `security/dsms/dsms_kernel_api.c:161`,
  `kernel/kmod.c:99`, `fs/coredump.c:652`, `init/do_mounts_initrd.c:79`
* Logs: `artifacts/device/logs/iter5-last_kmsg.txt`
* Binaries: `artifacts/known-good/iter5-fixed/vmlinux`
  (`out/vmlinux` sha256 `e25a276487fb7c65e956b89c07eb8f0ebe8a41415532f4b28c81ce0958a1dc2c`)
* Ramdisk: `artifacts/flash/05-halium-boot-skipinitramfs-noop.img`, ramdisk at file offset 48,525,312
