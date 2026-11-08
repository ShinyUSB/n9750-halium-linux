# Build environment: disk layout, container storage, and the "No space left" trap

Date: 2026-10-18

## The trap

The build runs inside the `kernel-build` distrobox container. That container's
root filesystem is a **podman overlay whose backing store is `/home`**
(`/dev/sdb2[@home]`). The kernel *output* directory is on a different
filesystem, so the two can disagree:

```
inside the container:
  /                   overlay   108G  106G  8.4M  100% /     <- podman overlay, on /home
  /home/usb           /dev/sdb2 108G  106G  8.4M  100%
  /home/usb/mnt-sda   /dev/sda2 220G  150G   70G   69% /     <- second disk
```

Symptom when `/home` fills:

```
fatal error: error in backend: IO failure on output stream: No space left on device
tee: /home/usb/mnt-sda/a6xx-build/build-gpu.txt: No space left on device
```

Both paths are on the *second* disk, which has 70 GB free. The failure is the
**overlay**, not the target filesystem: the compiler's temporary files go to
`/tmp` inside the container, which lives in the overlay on the full `/home`.

This is easy to misdiagnose as "the build dir is full". Check `df` *inside* the
container before believing any ENOSPC from a build:

```sh
distrobox enter kernel-build --no-tty -- bash -lc 'df -h / /home /home/usb/mnt-sda'
```

## Fixes applied

1. **`TMPDIR` points at the second disk.** `/home/usb/.opencode/build-gpu.sh`
   (and the working copy `/home/usb/.opencode-tmp/build-gpu.sh`) now exports
   `TMPDIR=/home/usb/mnt-sda/tmpbuild`. distrobox passes the host environment
   through, so this reaches the compiler inside the container. The directory is
   mode 777 so the container's mapped user can write it.

   Note: `/home/usb/mnt-sda/tmp` already existed, owned by root; the container
   cannot write it. Use a fresh name.

2. **Space freed on `/home`** (it was at 7 MB free — the host was about to
   wedge). Removed, all regenerable or scratch:

   | path | size | why safe |
   |---|---|---|
   | `n9750-halium/out/soong` | 2.1 G | soong build cache, regenerable |
   | `n9750-halium/cts` | 1.4 G | Android CTS, unused here |
   | `n9750-halium/out/build-lineage_halium_d2q.ninja` | 446 M | regenerable |
   | `n9750-halium/out/target` | 152 M | regenerable |
   | `n9750-halium/test` | 397 M | Android test suite, unused |
   | `.opencode/repack242*.img` | 153 M | scratch; `artifacts/flash/242-…` is canonical |
   | `.opencode/unpack201` | 155 M | scratch |
   | `.opencode-tmp/{rdcheck,rd52,img51,boot62..boot65}` | ~1.1 G | scratch extractions |

   **Kept:** `n9750-halium/out/host` (the `mkbootimg` binary that
   `pack-d2q.sh` requires), `artifacts/` (277 flash images + all project data),
   `.opencode/` (durable scripts and reference trees), both kernel trees, and
   `.opencode/linux-src` (mainline reference checkout).

   `/home` went from 7 MB free to ~3.1 GB free.

3. **Nothing was deleted from `artifacts/`.** That is the project archive.

## Disk map

| device | mount | size | free | holds |
|---|---|---|---|---|
| `/dev/sdb2` | `/home` (`@home` subvol) | 108 G | ~3.1 G | source trees, artifacts, podman overlay |
| `/dev/sdb2` | `/var/tmp` (`@tmp` subvol) | 108 G | same device | scratch |
| `/dev/sda2` | `/home/usb/mnt-sda` | 220 G | 68 G | **kernel build output**, `tmpbuild` |
| `/dev/sdc1` | `/run/media/usb/Ventoy` | 29 G | — | Ventoy USB |

`/home` and `/var/tmp` are two subvolumes of the same physical device, so
freeing one frees both. Keep at least a few GB of slack on `/home`; Btrfs
misbehaves badly when it is down to a few megabytes.

## Build command

Must be run **from the host** — it enters the container itself:

```sh
bash /home/usb/.opencode-tmp/build-gpu.sh
```

The container lacks `python2`; there is a shim at
`/home/usb/.opencode-tmp/bin/python2` -> `python2.7`.

kbuild is the only trustworthy compile oracle. An ad-hoc clang check once
reported a false "compiles clean" because the quoted include was unresolvable
from `/tmp` and it compiled nothing.
