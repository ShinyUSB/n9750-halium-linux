# Image 136 (build-only) — param fix ONLY

## The one diff (`a6xx_gmu.c`)
- Cut `module_param(secure_probe, int, 0444);` + its DESC from line ~1371
  (before prefix block) and pasted it after `gmu_hfi_probe`'s registration
  (line ~2404, under `MODULE_PARAM_PREFIX "a6xx."`). Variable untouched,
  default 0 untouched, mode logic untouched.
- Verified: exactly one registration tree-wide; registered name is now
  `a6xx.secure_probe`.

## Deliberate non-change (conflict resolved)
- PART 5's literal `SECVID136-BEFORE/AFTER-SKIP` strings NOT added: that
  would be a second source change, violating the image's defining
  constraint. Existing markers map 1:1 (snapshot=BEFORE, skip/write
  lines=AFTER, `[15]/[23]/[24]/[25]`=next operations). No CP_STATUS
  register exists in `a6xx.xml.h` (RBBM_STATUS used instead, as in 134).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- Packaged with 135 cmdline: probe=1 + secure=1 (+delay).
- Image 136 `136-halium-boot-a6xx-secvidskip1.img`
  sha256 `209d4063819bdf68a93e32945bd193bc8bd609860f847d89ee4440e0b3c0ca97`.

No flash performed; stopping (up to 5 cold-boot attempts after flash).
