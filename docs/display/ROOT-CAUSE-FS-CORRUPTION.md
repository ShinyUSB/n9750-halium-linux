# ROOT CAUSE FOUND — corrupted `/vendor` directory in the Halium rootfs

## The bug

`e2fsck` (host, 1.47.4) on `/userdata/rootfs.img` (2 GB ext4 inside the F2FS
`userdata` partition, sda30):

```
Invalid inode number for '.' in directory inode 2021.
Entry 'examples' in ??? (2021) is a link to directory /usr/share/doc/libreadline8/examples (2250).
'..' in /vendor (2021) is /usr/share/doc (2907), should be / (2).
```

**Inode 2021 is `/vendor`.** Its directory entries were destroyed.

The kernel's *first* firmware search path is `firmware_class.path=/vendor/firmware_mnt/image`
(from the cmdline). Every firmware attempt began by walking that broken directory,
producing:

```
EXT4-fs error (device loop0): __ext4_find_entry:1620: inode #2021: checksumming directory block 0
msm_drm: loading /vendor/firmware_mnt/image/qcom/a630_sqe.fw failed with error -74
```

That is why **every** firmware load failed with `-74`, and why the GMU blob load
wedged the device: the lookup was walking a corrupted directory on a loop-backed
filesystem. The `-74`s were never a missing-file problem.

Damage repaired: invalid `.` inode, cross-linked directory entry, wrong `..`,
extent tree, 5 inode refcounts, block/inode bitmaps, free block/inode counts,
directory counts. Post-repair verification pass: all 5 stages, no errors.

Repaired image md5 `22d1f009dce95a96a7b3ebccbb333a9d`, verified identical on
device. Corrupt original retained at `/tmpmnt/rootfs.img.corrupt`.
EXT4 errors after reboot: **0**; mount no longer warns "mounting fs with errors".

## What this re-explains

The unexplained **bootloops in images 67/68** were very probably this
corruption during GMU firmware load — not GMU init itself. The same corruption
was present in image 70, whose GMU probe happened to run at 340 s and so may
simply have not touched the bad path in the same way.

The disappearing `/root/gpu-trace.log` was also this: the directory write hit the
corrupt block, not a non-persistent filesystem.

## After the repair — both blobs load

```
msm_drm: loaded qcom/a630_sqe.fw from new location
msm_drm: loaded qcom/a640_gmu.bin from new location
```

No `-74`. First time the A640 GMU firmware has ever loaded on this device.

## Next, a precise and *different* failure

```
The GMU is not set up yet
WARNING: CPU: 3 PID: 1116 at a6xx_gmu.c:639 a6xx_gmu_resume+0x1e8/0x1f4
pc : a6xx_gmu_resume+0x1e8/0x1f4
lr : a6xx_gmu_resume+0x1e4/0x1f4
 a6xx_gmu_resume+0x1e8/0x1f4
 a6xx_pm_resume+0x18/0x34
 adreno_resume+0x1c/0x2c
a6xx_gpu_load_store+0x50/0x9c
Unable to handle kernel paging request at virtual address 00014650
Internal error: Oops: 96000045 [#1] PREEMPT SMP
```

`a6xx_gmu_resume()` line 639 is:

    if (WARN(!gmu->mmio, "The GMU is not set up yet\n"))
            return 0;

Faulting address `0x14650` is a register offset plus a NULL base — i.e. `gmu->mmio`
is NULL and the code dereferenced it anyway.

**This is an artefact of our own diagnostic deferral, not an A640 port bug.**
We deferred the GMU probe 300 s; the on-demand GPU core load ran while the GMU was
still unprobed, so `a6xx_gmu_resume()` had no MMIO. Upstream orders GMU setup
before GPU hw init.

## Implications for the deferral

Now that the filesystem is healthy, the immediate (non-deferred) GMU probe should
be retested directly — that is the configuration that bootlooped in images 67/68
and whose cause we now believe was the corruption.