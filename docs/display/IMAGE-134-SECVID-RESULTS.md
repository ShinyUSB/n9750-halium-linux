# Image 134 mode-0 run — vote TIMEOUT (A); SECVID unreached

## Vote classification: A) HFI_VOTE_TIMEOUT_OR_REJECTED
- Full ACK chain (1/4/3/11/14/10, error=0) at t=50s, then vote
  (`ack=1/freq=6/bw=0`, header `0x0070041e`) → 5s silence, `ret=-110`.
  No NACK packet (unlike PERF rejections), no ACK. POST/PLUS snapshots:
  SPTPRAC=0xc0, RPMHPWR/GXVOTE/MXVOTE 0, RB regs 0 — GX stayed off.
- Same bytes ACKed in the 133 run: firmware-side variance, second instance.

## Mode-0 SECVID question: UNANSWERED this run
- Vote failure aborts before hw_init (`suppressed after -110`), so the
  `SECVID134` snapshot never printed. Design note: with probe=1 gated
  in-cmdline, vote timeouts preempt the SECVID modes. A SECVID answer
  needs a vote-ACK run (firmware lottery) or probe=0.
- DCVS audit identical (`-110`, RETURN 0). Faults/panics/storm: 0.
  Device alive 177s+.

## Standing
- Mode 1 (skip) not yet run — requires reaching hw_init; same gating caveat.
- Mode 2 remains STOPPED (deps missing, per build record).
- Next: reboot + retry for a vote-ACK run, or run mode 1 with probe
  temporarily off-cmdline. Awaiting direction; no fixes applied.

Image 134 `3a34b983787be0c3b2cdbc8a72531f60123f3e68a9b2096a1463f49d851021f3`.
SDE 0, DT = 121, production untouched.
