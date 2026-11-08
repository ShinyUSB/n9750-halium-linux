# Image 113 — BW skipped: ECP also gets no response (B)

## Sequence (fresh boot, clean capture)

```
verify_fw_version -> ACK -> ret=0
send_dcvstbl     -> ACK -> ret=0
=== BW_TABLE SKIPPED FOR BRINGUP === -> ret=0 (skipped)
feature_ctrl ECP -> TIMEOUT (-110)
```

## Record (B)

- Next message after the skip: `HFI_FEATURE_ECP` (id 11, enable 0).
- No ACK in 5 s. No SMMU fault, no panic; device stable.
- `kgsl_hfi_irq` = 3 total (version ACK, perf ACK, +1 unidentified);
  `kgsl_gmu_irq` = 0.
- Whether the 3rd IRQ delivered an unmatched ECP response vs an unrelated
  async message is undetermined (RX decode filter excluded it this run).

## Reading

Skipping BW does not unblock the sequence — ECP fails identically. Two
consistent explanations, not distinguished: (a) firmware needs BW state
before answering anything further (skip breaks the chain); (b) ECP itself
is rejected/unsupported by this firmware build. Testing (a) requires a
working BW send, which is the blocked step — circular. Testing (b) requires
ECP-with-BW, equally blocked.

Image 113 `44c7ce11e2ec61b94b37c3be555c9b0da56010ac000c74ab8bd6991c91bc4d2b`.
SDE 0, DT +0, production untouched. Stopping.