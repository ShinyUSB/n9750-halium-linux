# Image 144 (build-only) — vitals +5s first poll + battery snapshot

## Why
- Battery at 17% / 3.677V while charging: silent power-cut resets fit
  better than any software cause (no record possible, TZ misclassifies).
- Vitals never fired (deaths <30s post-init): first poll now +5s.

## Diff (`a6xx_gpu.c` only)
- Vitals fn: battery snapshot (`battery` power_supply: capacity,
  voltage, temp, status; graceful NULL).
- Arming: first poll +5s, then 30s. All behavior otherwise identical.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0 (delta check in flash step).
- Image 144 `144-halium-boot-a6xx-vitals144.img`
  sha256 `97b1fce0d393848b92b52ce654c1a7b9daeac75b347ff4abeb598853edd01f40`.

## Operator request
Charge the phone well past 17% before trigger runs. If deaths stop
above ~60%, supply droop is confirmed as the reset class.
