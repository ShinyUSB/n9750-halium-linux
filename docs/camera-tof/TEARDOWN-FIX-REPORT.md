# CamX Teardown SIGABRT — Root Cause & Fix Report
**Date: 2026-11-01 · d2q (SM-N9750) Debian · camera bring-up, phase 2**

## TL;DR

The SIGABRT was **not** a missing `flush()`/`close()` (that was the initial hypothesis) and
not a dlclose issue. It is **Scudo detecting a corrupted heap chunk during process-exit
static teardown**, while 16 immortal CamX singleton worker threads are still alive. CamX's
static destructors race those threads; the vendor stack is architecturally not built for
`exit()`-style teardown — on stock Android these processes are SIGKILLed by init, so their
static destructors never run.

Fix = **full Camera3 lifecycle** (flush → close → verify counters → free shim resources)
**followed by `_exit()`** — the platform-equivalent termination. Result: 18/18 clean runs,
0 new Scudo errors, capture unaffected.

---

## 1. Root cause (evidence chain)

| Observation | Evidence |
|---|---|
| Abort is on the **main thread**, in libc's raise/tgkill path, during exit | GDB `catch signal SIGABRT`: thread 1, PC/LR both in `/opt/bionic32/libc.so`; earlier strace showed self-`SIGABRT` (SI_QUEUE) after `[shim] DONE` |
| Every abort is preceded by exactly one `Scudo ERROR: invalid chunk state when deallocating address ...` | camx.log Scudo count delta == abort count (5/5, 10/10); 18 clean runs added **0** |
| Corruption exists even with **no capture at all** | Test A (open/initialize/close only) aborts in the degraded regime → ion buffers/request structs not involved |
| 16 CamX worker threads **never exit** after close | thread census at close=18, 5s after close=16, unchanged; camx.log: CamX itself logs 7× "CheckJobsStillRegistered() … While closing camera, this job is not Unregistered" (NodeCommonThreadJobFamily, sak2l4sx, sak2l4sx_OIS, FDManagerThread, PerFrameDebugData, DeferredWorkerWrapper3, CHISessionWrapper2) |
| Not phone/driver state | 5/5 aborts immediately after a fresh reboot |
| Abort rate flips with binary rebuilds | same source, new build → clean→aborting flip; consistent with heap-layout-dependent corruption detection (Scudo detects damaged chunk headers at dealloc time, not corruption time) |
| Causality of the fix | same binary: mode `x` (no close, static destructors run) aborts 3/3; mode `c` (full lifecycle + `_exit`) clean 10/10 + 8/8 |

**Mechanism**: at `exit()`, `__cxa_finalize(NULL)` runs static destructors of every loaded
DSO — including `camera.qcom.so`'s CamX singletons — while CamX's immortal worker threads
still reference heap objects. A deallocation then hits a chunk whose header state is invalid
(race / double-free between destructor and worker) → Scudo reports and calls `abort()`.
Exact chunk attribution is impractical (stripped libc, tail-called raise path defeats
unwinding, Scudo string table referenced indirectly), but the mechanism is pinned by the
1:1 Scudo-error correlation, the thread census, and the causal A/B.

On stock Android this path never executes: `init` stops camera provider/cameraserver with
SIGKILL, so no vendor static destructor ever runs. Samsung never shipped a clean-exit path.

## 2. What the shim now does (cam-shim32.c)

1. `CAMERA_OPEN` → `initialize` (API ver 0x305, so `flush()` exists — checked, not assumed)
2. configure → N requests (`REQUEST_SUBMITTED`/`REQUEST_RETURNED` per call, counters)
3. Wait until `buffers_back == submitted` (every request's buffer returned by HAL)
4. `FLUSH_BEGIN/END` — rc=0, counters stable
5. `CAMERA_CLOSE_BEGIN/END` — rc=0, exactly once (guarded); no calls after close
6. 300 ms settle window (callbacks confirmed ceased; workers are immortal by design)
7. `SHIM_FREE` — free handle structs + ion fds **after** close returned
8. `PROCESS_EXIT` log with full counters → `_exit(rc)` (mode `x` keeps the old broken
   return path for forensics)

No dlclose anywhere (Test D: libraries stay mapped — grep confirms 0 occurrences).
No CamX destructors invoked manually, no assertions patched, no signal suppression.

## 3. Test results

| Test | Definition | Result |
|---|---|---|
| A | open/initialize/close | exit 0 (clean regime) |
| B | 1 capture + flush/close | exit 0, 2 results/1 buffer/1 notify |
| C | 4 captures + flush/close | exit 0 ×18/18 across builds; 4 req → 8 results/4 buffers/4 notify; frame dumped |
| D | C with libraries kept loaded | same as C (shim never dlcloses) |
| E | 2× full open/configure/capture/flush/close | exit 0 both cycles; CamX global state valid across sessions |
| x | 4 captures, no close, static teardown | exit 134 ×3/3 + Scudo error each (repro path preserved) |

Note on an earlier confusion: the first a/b/c/e runs exited 0 *with* plain `return` —
before later rebuilds flipped the heap layout. That intermittent period is what made the
bug look like "missing close fixes it". The 10/10 abort run with close+`exit()` proved
close alone is insufficient; `_exit` after the full lifecycle is the operative fix.

## 4. Lifecycle log of a fixed run (mode c)

```
[14837124] CAPTURE_RESULT  frame=4 outputs=1 (results=8 buffers_back=4)
[14837184] WAIT_DONE       submitted=4 results=8 buffers_back=4 notify=4
[14837280] FLUSH_BEGIN     outstanding: submitted=4 results=8 buffers_back=4
[14837281] FLUSH_END       rc=0 ...
[14837469] CAMERA_CLOSE_END rc=0 ... threads=16
[...]      THREADS_SETTLED  at_close=16 after_300ms=16 (immortal singletons)
[...]      SHIM_FREE        freeing 4 handles + ion fds
[...]      PROCESS_EXIT     rc=0 ... flush=1 close=1
[shim] DONE                      ← exit code 0
```

Capture correctness after the change: `frame0_after_teardown_fix.png` — real scene,
identical quality to pre-fix captures.

## 5. Files

- `cam-shim32.c` / `cam-shim32` (host + phone `/usr/local/bin/`) — instrumented shim, modes a/b/c/e/x
- `catch-scudo.py` — gdb syscall-catch experiment (write/writev/sendto/sendmsg)
- `/tmp/abort-dump.txt` (host copy) — registers/maps/stack at abort
- camx.log evidence lines: Scudo error + CheckJobsStillRegistered
