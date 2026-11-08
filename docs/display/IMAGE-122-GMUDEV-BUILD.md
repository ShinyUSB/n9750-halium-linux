# Image 122 (build-only) — GMU device-pointer ownership fix

## Exact source diff (`a6xx_gmu.c` only)
- `a6xx_gmu_memory_probe(struct device *mmu_dev, struct a6xx_gmu *gmu)`:
  removed `gmu->dev = gmu->cb_dev`; `mmu_dev` used for the two log lines.
- Call site passes `&adreno_gpu->base.pdev->dev` (kgsl-3d0 GPU device,
  valid: `gpu->pdev` set in `msm_gpu_init` before GMU probe) + new
  `GMU_DEV: before/after` prints (ptr/name/of_node, plus cb name after).
- Error path: `iommu_detach_device(domain, gmu->cb_dev)` (was `gmu->dev`;
  restores attach/detach symmetry).
- `pwrlevels_probe`: added `OPP122:` summary
  (gmudev/probe_ret/counts/rpmh_ret); OPP121 + PERF120 dumps retained.
- Untouched: DT (121 content byte-identical), HFI/queues/ACK, PERF/BW/ECP
  payloads, firmware, clocks/power, OOB/DCVS, hw_init.

## Ownership old vs new
- old: `gmu->dev` = GMU pdev until `memory_probe`, then CB device for all
  later code (clocks_probe ran before the switch, so only OPP/PM/buffers
  saw the CB device; buffer map is domain-based, device-independent).
- new: `gmu->dev` = GMU pdev throughout; CB device only in `gmu->cb_dev`
  (+ explicit `mmu_dev` param). Side effect (intended, upstream-shaped):
  `pm_runtime_*` in resume/suspend now act on the real GMU device
  (previously on the CB device, likely no-ops).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT unchanged since 121; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 122 `122-halium-boot-a6xx-gmudev.img`
  sha256 `4332bb566ecf557603a4fa65e4c55d92bc44a3c05049817643b5499fb227207a`.

No flash performed; stopping.
