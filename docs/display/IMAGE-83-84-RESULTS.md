# Images 83/84 — allocation fix validated, CX vote works, instability persists

## Validated

| item | evidence |
|---|---|
| Allocation fix live | `GMU clocks: count=5 array=80 bytes (16 each)` |
| CX regulator exists, was OFF | `before CX GDSC enable (cx=ffffffc3634a2480 already=0)` |
| CX vote succeeds | `after CX GDSC enable (ret=0 already=1)` |
| All 5 clocks prepare ret=0, all 5 enable reached | image 84 run 1 (pass 1 all ret=0, pass 2 started) |
| Both firmware blobs loadable | `fw_direct returned 'qcom/a640_gmu.bin' ret=0` + `loaded ... from new location` (image 84 run 1) |
| PDC routing fix | source-verified vs v5.8 + KGSL; **never executed** (no boot reaches rpmh_init) |

## Not met: deterministic execution

Deaths across images 83/84 triggers, all silent (no Oops/panic/pstore/EXT4),
fast (<60s), device reboots:

| run | last marker |
|---|---|
| 83-T1 | `fw_direct enter 'qcom/a640_gmu.bin'` |
| 83-T2 (hang, alive) | `requesting fw[1]` |
| 83-T3 | `requesting fw[0] 'a630_sqe.fw'` |
| 84-T1 | `--- pass 2: clk_enable ---` (start) |
| 84-T2 | `fw_direct enter 'qcom/a630_sqe.fw'` |

## Ruled out this round

* **fw_lock contention with boot activity**: late trigger at uptime 327 s with
  zero firmware activity in the prior 60 s died identically.
* **SSH killing the task**: detached (`setsid`) triggers reboot identically.
* **On-device capture**: impossible. `/data` is wiped on reboot; `/userdata`
  (sda30 F2FS) rolls back via `rootfs.img.part` recovery — files created at
  13:15 vanish while 13:14 files survive; `rootfs.img` mtime rewinds to 13:17.
  Host-side unbuffered streaming (`grep --line-buffered /dev/kmsg`, no `cat`
  pipe) is the only channel. Also fixed this round: the `cat | grep` pipe was
  eating 4KB blocks of crash lines in its buffer.
* **ftrace**: only `nop` tracer available.

## Leading hypothesis: power-rail sequencing (PMIC protection)

A random-point silent fast reset with no kernel involvement is the signature of
a PMIC-level shutdown (OCP/UVLO/brownout), not a software bug. Supporting notes:

* Our resume enables CX + 5 clocks while **GX (`"vdd"`, owned but never
  enabled in resume) stays off**. KGSL powers GX via pwrctrl/RPMh votes; our
  `a6xx_gmu_gfx_rail_on()` only runs much later in `fw_start`.
* The instability window (probe → clocks) is exactly where rail load steps up.
* Image 78 (no CX vote) reached rpmh_init; images with CX vote die earlier —
  not because CX is wrong (it returns 0) but because each added load step
  shifts where the rail gives way. This also explains why the point moves.

This is hypothesis, not finding. The alternative (second slab bug elsewhere on
the path) is not ruled out.

## Proposed next step

Add the GX-side vote to match KGSL ordering before clocks — either
`regulator_enable(gmu->gx)` in resume (minimal) or the RPMh gfx-rail vote —
then re-test for determinism. If deaths stop moving and execution reaches
rpmh_init, the PDC fix finally gets its runtime validation.