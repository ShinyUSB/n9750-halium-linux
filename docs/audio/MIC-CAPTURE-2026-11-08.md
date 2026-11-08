# d2q mic capture bring-up — 2026-11-08 (ALL THREE MICS WORKING ✅)

Device: SM-N9750 (d2q) · Debian 12 · kernel 4.14.190-ShinyKernel #126 · card
`sm8150-tavil-snd-card`. Follows AUDIO-BRINGUP.md (playback done 2026-11-04).
No kernel/DT changes needed — pure mixer routing, all stock-faithful from the
vendor XML.

## Ground truth source

`/vendor/etc/mixer_paths_tavil.xml` (stock vendor still mounted RO on Debian —
sda25). The sound node DT (`mixer-paths = "mixer_paths_tavil_old.xml"`) points
at the old variant, but tavil.xml carried the same TX paths. Host copy:
`mnt-sda/audio-recon/mixer_paths_tavil.xml` (136 KB).

## The three mics (channel map)

| mic | stock path | codec route | SLIM TX | bias | location |
|---|---|---|---|---|---|
| main | `main-mic`  | DMIC0 → DEC5 | TX5 | MIC BIAS1 | bottom, primary |
| sub  | `sub-mic`   | DMIC2 → DEC6 | TX6 | MIC BIAS3 | top |
| 3rd  | `3rd-mic`   | DMIC5 → DEC7 | TX7 | MIC BIAS4 | third |

**The "4th-mic" is a phantom**: DMIC4/DEC8/TX8 is a MIRROR of DMIC5 — recorded
4ch during the 1 kHz speaker-tone test and cross-correlated:
corr(ch2 DMIC5, ch3 DMIC4) = **1.0000** (ch0/ch1/ch2 mutually independent).
Stock also mutes it (`DEC8 Volume` = 0 while DEC5/6/7 = 84). Note 10+ has
exactly 3 mics, matching the user's count.

## What was applied (all runtime, no flash)

FE side (`audio-record` path): `MultiMedia1 Mixer SLIM_0_TX` = 1.
Codec side per mic (example main): `AIF1_CAP Mixer SLIM TX5`=1,
`CDC_IF TX5 MUX`=DEC5, `ADC MUX5`=DMIC, `DMIC MUX5`=DMIC0.
Port width: `SLIM_0_TX Channels` = One (single) / Four (multi).
No manual MIC BIAS csets — bias comes up via the card's qcom,audio-routing
DAPM pairs (DMIC0↔MIC BIAS1, DMIC2↔MIC BIAS3, DMIC5/4↔MIC BIAS4).

Capture device = FE **hw:0,0** (MultiMedia1) → BE SLIMBUS_0_TX → tavil.

## Evidence (1 kHz tone through speakers, measured on host)

- DMIC0 mono, tone amp at 1 kHz ≈ 9982 vs ambient 3–113 → live.
- 4ch multi: ch0 amp 11721 / ch1 4863 / ch2 98176 / ch3 98175 (ch2/ch3 mirror).
- dmesg: zero q6afe/slimbus errors during all captures. First-try success.

## Persisted on phone

- `/usr/local/bin/d2q-mic-init.sh` [main|multi] — stock-faithful route applier
  (host copy: mnt-sda/audio-recon/d2q-mic-init.sh)
- `d2q-mic.service` — oneshot at boot, After=d2q-audio-init, mode main (enabled)

## Verified after persist

main-mode mono 3 s capture: peak 394, RMS blocks 35–84 (live room noise);
speaker playback unaffected (`aplay` OK after route changes).

## Traps (new)

- ssh one-liners: `\$n` in a compound remote loop did NOT expand (12 amixer
  "Cannot find the given element") — write literal per-index commands, or scp a
  script file (loops fine inside a real file).
- amixer control names are exact-string; enum values too (`DEC5` not dec5).

## Not done / next

- USB-C headset mic (AMIC2 + MIC BIAS2, fsa4480 muxing) — path exists in XML,
  not tested (no headset priority).
- Voice/VoIP FEs (VoiceMMode1_Tx Mixer SLIM_0_TX_MMode1) untested — calls.
- PulseAudio/PipeWire layer still pending (from AUDIO-BRINGUP.md); it will see
  hw:0,0 capture directly.
- Subjective (human) speech check pending — objective tone test passed all mics.
