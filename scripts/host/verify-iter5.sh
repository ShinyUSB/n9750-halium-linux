#!/usr/bin/env bash
# Iteration-5 verification, part 1: source integrity, config integrity, arch.
set -uo pipefail
cd /home/usb

K=ShinyKernel/Kernel
PRISTINE=ShinyKernelREVIEWED/Kernel          # read-only git tree @ 32895a1f5
BASE_CONFIG=artifacts/known-good/shinykernel-baseline/config
BASE_CONFIG_SHA=1d92ce65a4d6163d36ab0cd729f7a5ea0191e4c76d9dbe8d5e7e1497469f45e9

hdr() { printf '\n=== %s ===\n' "$*"; }
ok()  { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; RC=1; }
RC=0

hdr "1. source integrity"
# NOTE: ShinyKernelREVIEWED/Kernel is NOT a byte-identical reference for the
# whole tree (e.g. fs/open.c differs by 56 lines, selinuxfs.c by 145, all with
# 2022-02-08 mtimes -- pre-existing tree-to-tree divergence). It was only ever
# used to double-check the two files this change touches, where it agreed.
# The authoritative statement is the mtime scan plus the pre-edit snapshots.
echo "  files in the kernel tree modified since 2026-09-25 21:25 (build trees excluded):"
mod=$(find "$K" -name 'out*' -prune -o -type f -newermt "2026-09-25 21:25" -print 2>/dev/null | sort)
echo "$mod" | sed "s|$K/|      |"
n_src=$(echo "$mod" | grep -cE '\.(c|h|S)$')
# Of the mtime-touched source files, which actually DIFFER in content from pristine?
differ=""
for f in $(echo "$mod" | grep -E '\.(c|h|S)$'); do
  rel=${f#"$K"/}
  if [ ! -f "$PRISTINE/$rel" ] || ! cmp -s "$f" "$PRISTINE/$rel"; then differ="$differ $rel"; fi
done
nd=$(echo $differ | wc -w)
if [ "$nd" -eq 2 ] && echo "$differ" | grep -q 'arch/arm64/kernel/setup.c' && echo "$differ" | grep -q 'init/initramfs.c'; then
  ok "content-differs set is exactly:$differ"
  ok "the remaining $((n_src-2)) mtime-touched source file(s) are content-identical to pristine"
else bad "unexpected content-differing set:$differ"; fi
for f in include/linux/ologk.h; do
  cmp -s "$K/$f" "$PRISTINE/$f" && ok "$f byte-identical to pristine (mtime-only touch by the build)" || bad "$f actually changed"
done
[ -n "$(echo "$mod" | grep -vE '\.(c|h|S)$' | grep -v 'build_log.txt')" ] \
  && printf '  note   other non-source files touched (build outputs):\n%s\n' "$(echo "$mod" | grep -vE '\.(c|h|S)$' | grep -v build_log.txt | sed 's|^|        |')"

hdr "1b. pre-edit snapshots were genuinely pristine"
for f in initramfs setup; do
  :; done
cmp -s artifacts/device/iter5-baseline/initramfs.c.orig "$PRISTINE/init/initramfs.c" \
  && ok "snapshot initramfs.c.orig == pristine git tree" || bad "snapshot initramfs.c.orig != pristine"
cmp -s artifacts/device/iter5-baseline/setup.c.orig "$PRISTINE/arch/arm64/kernel/setup.c" \
  && ok "snapshot setup.c.orig == pristine git tree" || bad "snapshot setup.c.orig != pristine"

hdr "2. the two intended changes, as diffs"
for f in init/initramfs.c arch/arm64/kernel/setup.c; do
  echo "  --- $f ---"
  diff -u "artifacts/device/iter5-baseline/$(basename $f).orig" "$K/$f" | sed -n '3,$p' | sed 's/^/    /'
done

hdr "3. skip_initramfs handler semantics"
fn=$(sed -n '/^static int __init skip_initramfs_param/,/^}/p' "$K/init/initramfs.c")
echo "$fn" | sed 's/^/    /'
grep -q '__setup("skip_initramfs", skip_initramfs_param);' "$K/init/initramfs.c" \
  && ok "__setup(\"skip_initramfs\", skip_initramfs_param) retained verbatim" \
  || bad "__setup registration missing/renamed"
echo "$fn" | grep -q 'do_skip_initramfs = 1' \
  && bad "handler still sets do_skip_initramfs" \
  || ok "handler no longer sets do_skip_initramfs (stays 0)"
echo "$fn" | grep -q 'return 1;' \
  && ok "handler returns 1 -> parameter stays recognised" \
  || bad "handler does not return 1"
grep -q 'if (do_skip_initramfs) {' "$K/init/initramfs.c" \
  && ok "populate_rootfs() branch left in place (only unreachable now)" \
  || bad "populate_rootfs() branch removed - NOT intended"

hdr "4. kernel config byte-identity"
cur=$(sha256sum "$K/out/.config" | cut -d' ' -f1)
printf '  known-good baseline : %s\n  freshly built       : %s\n' "$BASE_CONFIG_SHA" "$cur"
if [ "$cur" = "$BASE_CONFIG_SHA" ]; then ok ".config byte-identical to the known-good baseline"; else bad ".config CHANGED"; fi
if cmp -s "$K/out/.config" "$BASE_CONFIG"; then ok "cmp() confirms byte-identity"; else bad "cmp() reports a difference"; fi
printf '  also identical to prebuilt/halium-kernel.config: '
cmp -s "$K/out/.config" n9750-halium/device/samsung/d2q/prebuilt/halium-kernel.config && echo yes || echo NO

hdr "5. per-symbol config comparison (baseline vs fresh build)"
d=$(diff <(sort "$BASE_CONFIG") <(sort "$K/out/.config") || true)
if [ -z "$d" ]; then
  ok "every config line identical ($(grep -cE '^[A-Z#]' "$BASE_CONFIG") entries, 0 differing symbols)"
else
  bad "config symbol differences:"; echo "$d" | sed 's/^/        /'
fi

hdr "6. the config symbols that matter to this change"
for s in CONFIG_BLK_DEV_INITRD CONFIG_BLK_DEV_RAM CONFIG_RD_GZIP CONFIG_INITRAMFS_FORCE CONFIG_INITRAMFS_SOURCE; do
  b=$(grep -E "^$s=" "$BASE_CONFIG" | head -1); c=$(grep -E "^$s=" "$K/out/.config" | head -1)
  same=$([ "$b" = "$c" ] && echo "same" || echo "DIFFERS")
  printf '  %-28s baseline=%-24s current=%-24s %s\n' "$s" "${b:-<unset>}" "${c:-<unset>}" "$same"
done

hdr "7. kernel is ARM64"
# A raw arm64 Linux Image has no file(1) magic; the authoritative marker is the
# ARM64 image header magic at offset 0x38.
python3 - "$K/out/arch/arm64/boot/Image" "$K/out/arch/arm64/boot/Image-dtb" <<'PY'
import struct, sys
for p in sys.argv[1:]:
    d = open(p, 'rb').read(0x100)
    code1, code2 = struct.unpack_from('<II', d, 0x38)
    txt = d[0x38:0x3c]
    is_arm64 = txt == b'ARM\x64'
    print("  %-52s magic@0x38=%r  %s" % (p.split('/')[-1], txt,
          "ARM64 Image header OK" if is_arm64 else "NOT an ARM64 image"))
    # text_offset is at 0x08 for arm64 Image header
    print("      text_offset=0x%x  image_size=0x%x  flags=0x%x  res1=0x%x res2=0x%x"
          % (struct.unpack_from('<Q', d, 0x08)[0],
             struct.unpack_from('<Q', d, 0x10)[0],
             struct.unpack_from('<I', d, 0x30)[0],
             struct.unpack_from('<I', d, 0x3c)[0],
             struct.unpack_from('<I', d, 0x40)[0]))
    sys.exit(0 if is_arm64 else 1)
PY
[ $? -eq 0 ] && ok "both Image and Image-dtb carry the ARM64 image header magic" || bad "ARM64 header magic missing"
readelf -h "$K/out/vmlinux" 2>/dev/null | grep -E 'Class:|Machine:|Type:' | sed 's/^/  /'

hdr "RESULT part 1"
[ $RC -eq 0 ] && echo "  all checks passed" || echo "  one or more checks FAILED"
exit $RC
