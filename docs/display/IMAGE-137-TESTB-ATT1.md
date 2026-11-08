# IMAGE-137 TEST B attempt 1 — hfi failed before vote (firmware variance)

## Result
- `NONLEGACY137: legacy DCVS set_freq skipped at boot` logged (the skip
  itself works).
- Message-hfi silent 5s (no ACKs at all) → `-110` → suppress. Vote never
  attempted; `set_freq` correctly NOT called (no DCVS lines).
- Log-archaeology note: persistent-log timestamps repeat across boots;
  attribution verified by line order (this boot = lines 366–423 only).
  An earlier misread of 136-boot lines as current-boot is corrected here.

## Classification: none yet (vote untested)
- Not NONLEGACY_HFI_VOTE_FAILS (no vote sent); hfi-level `-110` before it.
- DCVS-skip leg: proven (logged). POSTINIT: unreached.

## Standing
- Attempt 1 of up to 5. Device alive 257s+, faults 0.
- Next: cold reboot + trigger (attempt 2).

## Attempt 2 (same image/cmdlne)
- GOOD BABEFACE; register-hfi ok; message-hfi 5s silent → -110.
- `NONLEGACY137 skip` logged; `set_freq` absent (correct); suppress.
- Vote untested again. Unexplained ~60s trigger-to-fw_start delay noted
  (armed ~33s, fw_start 96s). Device alive, faults 0.

## Attempt 3
- Full ACK chain (1/4/3/11/14/10, error=0) → vote SENT
  (`ack=1/freq=6/bw=0`, header `0x0070041e`) → 5s silence → `ret=-110`
  (`Message HFI_H2F_MSG_GX_BW_PERF_VOTE id 7 timed out`).
- POST/PLUS snapshots: SPTPRAC=0xc0, all-zero GX, RB regs 0 (unchanged).
- DCVS skipped, suppress. No storm/panic. Device alive.

## Attempt 4
- `FORENSIC VERDICT BAD BADDFACE ms~1` → `fw_start ret=-5` → suppress.
  Pre-HFI firmware failure (first BADDFACE since image 117).
- No HFI, no vote, no storm. Gate held, device alive.
