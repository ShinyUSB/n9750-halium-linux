# Image 128 — ADDR64 lands cleanly; CP still never fetches (new datum)

## ADDR64 + path
- All 12 writes completed (`[00]` CP → `[11]` SECVID_TSB), `[08]`
  STATUS=0. GBIF clean (`0x71620` ×4, QOS reads 0x0 again),
  `adreno_hw_init ret=0`, SQE_CNTL written. No write hung.

## CP verdict (unchanged failure, sharper datum)
- `[11]` cp_init → 1.0s → **`CPIDLE-FAIL: STATUS=0x0 INT0=0x0
  CP_RB_RPTR=0x0 CP_RB_WPTR=0x0`** → `[12] ret=-22` → `[25] return -22`.
- Both HW pointers zero after flush: the CP never fetched the ring
  (not a slow-idle — a never-started). GPU fully idle, no IRQ, no fault.
- ADDR64 hypothesis for the idle failure: ruled out by experiment.

## Storm timing (new datum: onset B)
- NACK storm ran DURING bootstrap (error=111 interleaved from seq=1,
  new ids 248 seen) — yet all six expected ACKs still arrived cleanly
  and `hfi_start ret=0`. Reader walks both.
- Silent reboot followed within ~2 min of `hw_init ret=-22`. No panic/
  Oops/SMMU fault on record. Device alive on fresh boot.

Image 128 `20a95a9fbb182775837f9b29652d58d32d7fac2b36b94b0abeac104d105c3c73`.
SDE 0, DT = 121, production untouched. Stopping.
