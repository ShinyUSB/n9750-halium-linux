# The HFI measurement harness was measuring the wrong thing

Image 198. The tally says `OK=0 TIMEOUT=0 ABSENT=3`, which directly contradicts
the first boot after flashing:

```
[36.141240] A6XX_DIAG: sptprac_enable -> skipping ...
[36.141524] HFI-ACK id=1  error=0
[36.141685] HFI: verify_fw_version ret=0
... 6 ACKs total, all error=0
```

Both cannot be true, so one of the two measurements is wrong. It is the harness.

## Root cause: bring-up is TIME-BASED, and the capture starts too late

Bring-up happens at kernel timestamp **~36 s into boot** — a fixed time, not
something the trigger initiates. The measurement harness, however:

1. waits for the phone to be reachable — it came up at **uptime ≈ 133 s**
2. *then* starts the durable capture
3. *then* fires the trigger

By step 2 the bring-up has already happened and been discarded. `dmesg -w` only
reports messages emitted **after it attaches**, so the entire bring-up is invisible
to it. That is why every attempt reads `ABSENT` with `acks=0` AND
`hw_init_suppressed=0` — the double-zero is the tell: if bring-up had run and
failed we would see `hw_init_suppressed=1`, as in the earlier TIMEOUT attempts.

So `ABSENT` here means **"the harness looked in the wrong place"**, not "bring-up
did not happen".

This also means the earlier `HFI-MEASUREMENT-RESULTS.md` conclusion — "3 of 4 boots
never reached HFI" — is **not safely supported**. That measurement had the same
capture-timing flaw. The conclusion happened to point at sptprac, which image 198
then fixed, so it led somewhere useful, but the evidence for "reached HFI ~1 boot
in 4" was never actually measuring that.

## Two lessons, both the same shape

1. `dmesg -w` is a *streaming tail*, not a history. Attaching late silently loses
   everything before the attach, with no error.
2. `ABSENT` was treated as a GPU verdict when it can equally mean "harness never
   looked". A diagnostic that can report either cause without distinguishing them
   is not a diagnostic.

## Correct method

- Start the capture **before** bring-up — i.e. as early as possible after boot,
  well before the ~36 s mark.
- Or use `dmesg` (non-following, reads the whole ring) after the fact, since the
  ring survives until reboot. `dmesg` at ~133 s would have contained the whole
  bring-up.
- Treat `ABSENT` as *inconclusive* unless the capture demonstrably spanned t=0.

## State

- Image 198's sptprac guard is still supported by the one clean observation
  (6 ACKs, `ret=0`, first boot after flash), but has **not** been measured across
  boots.
- No rate is established either way.
- The liveness gate has still never passed on any image.
- Pixels remain unproven.

The next measurement should start the capture early, or simply read `dmesg`
outright, and it should verify that `hw_init` ran (`sw_rb_iova != 0`) before
recording any verdict.