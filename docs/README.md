# Documentation index

- `display/` — the GPU/display bring-up corpus. Start with `A6XX-FEASIBILITY.md`, then the
  STATE/RESULTS notes in date order. `patches/kernel/01-display-d2q.patch` is the resulting
  kernel delta. The d2q display "kick" mechanism (why Xorg works, why wlroots stalls) is
  described in the audited notes and in the reference files.
- `audio/` — three sessions: ADSP/PDR plumbing (AUDIO-BRINGUP), the three microphones
  (MIC-CAPTURE: DMIC0/2/5 with the mirrored 4th proven by cross-correlation), and the
  PipeWire/desktop layer (PIPEWIRE-PHOSH: labwc, wlroots 0.17 Vulkan renderer, the
  proc-superblock leak workaround, volume pins).
- `camera-tof/` — 4K60 root cause (Samsung client-identity rawParam), ToF depth engine.
- `telephony/` — IPA v4.1 + QMI data path: what was fixed (provider registration, UL rules)
  and the open DSD hypothesis, with every eliminated hypothesis documented.
- `evidence/` — raw kernel-side samples from the live device freeze investigation:
  `fw.log` = crtc_commit thread stacks every 4 s; `lw.log` = all labwc threads' states.
  These were captured on the 305 kernel during real panel stalls and show that the commit
  threads stay idle (compositor stops submitting) while the panel holds frames — the
  failure classes to investigate are documented in the sessions doc.
