# wptr==0 answered: the ring is never programmed. `a6xx_cp_init` is not reached.

## The measurement

With the GPU genuinely up (`GL_RENDERER == FD640`, asserted in the same run),
a hand-built submit — the exact test that proved image 189 — succeeds:

```
GEM_NEW          rc=0    handle=1 size=256
GEM_INFO         rc=0    offset=0x102166000
GEM_SUBMIT       rc=0    returned fence=1
SYNCOBJ_CREATE   rc=0
```

Then, immediately after:

```
CPDUMP sw : rb_iova=0x0 rb_size=32768 wptr_dw=0 next_dw=0
CPDUMP hw : RB_BASE=0x0 RB_CNTL=0x0 RB_RPTR_ADDR=0x0
CPDUMP hw : RB_RPTR=0x0 RB_WPTR=0x0 SQE_CNTL=0x0 SQE_BASE=0x0
CPDUMP st : RBBM_STATUS=0x0 linux_irq=487 irqs=0
CPF: t0=0x0 t1=0x0 delta=0 => CP is NOT CLOCKING
```

**`rb_iova=0x0`.** The ring has no software address at all. That is not a
"submitted but not executed" state — the ring was never set up.

## Comparing against a boot that DID program the ring

The driver already emits a `RINGHW` diagnostic for exactly this. From
`crashes/last_kmsg-1010-001228-ff5bf99a.log`:

```
RINGHW-W: CP_RB_CNTL before=0x0
RINGHW-W: CP_RB_CNTL after=0x20c
RINGHW-W: CP_RB_BASE before=0x0:00000000 val=0x1000
RINGHW-W: CP_RB_BASE after=0x0:00001000
RINGHW-W: CP_SQE_INSTR_BASE before=0x0 val=0x9000
RINGHW-W: CP_SQE_INSTR_BASE after=0x9000
RINGHW[00] SQE_INSTR_BASE=iova:0x9000 hw=0x9000
RINGHW[01] CP_RB_BASE=iova:0x1000 hw:0x1000
RINGHW[02] CP_RB_CNTL hw=0x20c
RINGHW[04] CP_SQE_CNTL = 1
RINGHW[05] a6xx_cp_init enter
```

In a working boot: ring programmed at iova `0x1000`, `RB_CNTL=0x20c`,
SQE at `0x9000`, and `a6xx_cp_init` is entered.

**Today on image 192: `rb_iova=0x0`, no `RINGHW` lines at all, `a6xx_cp_init`
never reached.**

## Root cause

**`adreno_hw_init()` — which is what allocates the ring and writes `RB_BASE`,
`RB_CNTL` and `SQE_INSTR_BASE` — is not completing on the current boots.** The
submit ioctl returns `rc=0` because it only validates BO handles and flags and
copies the command stream; it never checks that the ring exists. So "submit
accepted" and "GPU can execute" are independent, and only the first is currently
true.

This also explains the earlier `CP is NOT CLOCKING` verdict: with `RB_BASE=0`,
the CP has nothing to fetch from, so it never starts clocking. That was always a
*symptom* of the missing ring, not a separate fault.

## Relationship to the RINGHW=0 observation

`RINGHW` never appears in the image-192 logs at all (`grep -ac RINGHW` = 0 on
the boots I captured). Since `RINGHW[00..05]` are unconditional `pr_info`s inside
the ring-programming path, their total absence means that code path was never
entered — which is consistent and independently corroborates
`adreno_hw_init()` not running.

## Honest gap

`adreno_hw_init()` is reached in `a6xx_gpu_init()` between `HWINIT[19]` and
`HWINIT[20]`. I have **not** yet read its return path or the `HWINIT[19]/[20]`
lines on a current boot, because those boots keep ending in the display-triggered
reboot before I can capture them. So the precise failure inside
`adreno_hw_init()` — early `goto out`, or an error from a preceding step — is
still unproven.

The vblank reset actively blocks this: it gives roughly one clean boot, and the
ring-programming path needs a *successful* bring-up plus a submit inside that
window.

## Next step

1. Capture `HWINIT[18]`→`[21]` and the `adreno_hw_init` return on a boot where
   the GPU comes up. That is a one-line diagnostic and it names the failing step.
2. Given the vblank reboot costs ~5 min per boot and blocks this, consider
   suppressing the display timeout first — it is display-side, unrelated to the
   GPU, and it is now the bottleneck on investigation itself.

Do not touch the submit path (190/191) or the ITCM hole (192). Both verified.