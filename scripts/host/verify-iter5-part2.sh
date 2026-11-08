#!/usr/bin/env bash
# Part 2 of the iteration-5 verification: build outputs and BOOT image.
set -Eeuo pipefail
cd /home/usb

K=ShinyKernel/Kernel
BASE_KERNEL_SHA=7dedfea614d179fa81b44ecd3e8c13d5285a7267a1846f6d0631f8a74af8e263
BASE_ITER2_SHA=a5e3d8b240a204171543183141a46f58fae9222c313d743c6dc3614eab201d00
BASE_RAMDISK_SHA=0bbac4577f3567aec935c958216de5d30c7355452ca56248d6728f4f2634bdb6
ITER2=artifacts/flash/03-halium-boot-baseline-kernel-ramoops-cmdline.img
NEW_IMG=${1:-artifacts/flash/04-halium-boot-skipinitramfs-noop.img}

hdr() { printf '\n=== %s ===\n' "$*"; }
ok()  { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; RC=1; }
RC=0

IMG=$K/out/arch/arm64/boot/Image-dtb
VL=$K/out/vmlinux

hdr "6. diagnostic string present, normal unpack path retained"
# grep -aF: the literal contains [ % ] which are BRE metacharacters
if grep -aqF -- "BootLoader command line: [%s]" "$VL"; then ok "vmlinux contains the diagnostic string"; else bad "diagnostic string MISSING from vmlinux"; fi
if grep -aqF -- "BootLoader command line: [%s]" "$IMG"; then ok "Image-dtb contains the diagnostic string"; else bad "diagnostic string MISSING from Image-dtb"; fi
for s in "Trying to unpack rootfs image as initramfs..." "rootfs image is not initramfs" "skip_initramfs" "do_skip_initramfs" "populate_rootfs" "BootLoader command line"; do
  printf '  %-46s vmlinux=%-3s Image-dtb=%s\n' "$s" "$(grep -acF -- "$s" $VL)" "$(grep -acF -- "$s" $IMG)"
done
if grep -aqF -- "Trying to unpack rootfs image as initramfs..." "$IMG"; then
  ok "normal initramfs unpack path still compiled in (we only changed the skip handler)"
else bad "normal unpack path string MISSING"; fi
if grep -aqF -- "skip_initramfs" "$IMG"; then
  ok "skip_initramfs still a recognised parameter (not 'Unknown kernel command line')"
else bad "skip_initramfs registration gone"; fi

hdr "6b. compiled behaviour of the skip handler (binary proof)"
python3 - <<'PY'
import subprocess, re
V="/home/usb/ShinyKernel/Kernel/out/vmlinux"
nm=subprocess.run(["nm","-n",V],capture_output=True,text=True).stdout
names={l.split()[2]:int(l.split()[0],16) for l in nm.splitlines() if len(l.split())==3}
addrs=sorted(names.values())
a=names["skip_initramfs_param"]; end=addrs[addrs.index(a)+1]
out=subprocess.run(["/usr/bin/llvm-objdump","-d","--no-show-raw-insn",
                    "--start-address=0x%x"%a,"--stop-address=0x%x"%end,V],
                   capture_output=True,text=True).stdout
ins=[l.split("\t",1)[1].strip() for l in out.splitlines() if re.match(r'^[0-9a-f]{16}:',l)]
print("  skip_initramfs_param: %d bytes, %d instruction(s)"%(end-a,len(ins)))
for x in ins: print("      ",x)
body=" ".join(ins)
good = ("mov" in body and "#0x1" in body) and "ret" in body and "str" not in body
print("  => compiles to 'return 1' only, no store to do_skip_initramfs:", "YES" if good else "NO")
print("  do_skip_initramfs still in symtab:", "do_skip_initramfs" in names,
      "(absent => compiler proved it dead and removed the whole skip branch)")
PY

hdr "7. built-in initramfs payload still the 512-byte stub"
ls -l "$K/out/usr/initramfs_data.cpio" 2>/dev/null | sed 's/^/  /'
cpio -itv < "$K/out/usr/initramfs_data.cpio" 2>/dev/null | sed 's/^/    /'
printf '  sha256: %s\n' "$(sha256sum $K/out/usr/initramfs_data.cpio | cut -d' ' -f1)"

hdr "8. DTB / DTBO outputs"
printf '  d2q DTBO files: %s (expected 15)\n' "$(find $K/out/arch/arm64/boot/dts/samsung/davinci -name '*.dtbo' -type f 2>/dev/null | wc -l)"
ls -l "$K/out/arch/arm64/boot/dts/qcom/sm8150.dtb" "$K/out/arch/arm64/boot/dts/qcom/sm8150-v2.dtb" 2>/dev/null | sed 's/^/  /'

hdr "9. build output hashes"
sha256sum "$K/out/arch/arm64/boot/Image" "$IMG" "$K/out/.config" | sed 's/^/  /'
printf '  baseline Image-dtb: %s\n' "$BASE_KERNEL_SHA"

hdr "10. BOOT image: $NEW_IMG"
ls -l "$NEW_IMG" | sed 's/^/  /'
sha256sum "$NEW_IMG" | sed 's/^/  /'
printf '  iteration 2 image : %s\n' "$BASE_ITER2_SHA"
python3 - "$NEW_IMG" "$ITER2" <<'PY'
import struct, hashlib, sys
def geom(p):
    d = open(p, 'rb').read(16384)
    g = lambda off: struct.unpack_from('<I', d, off)[0]
    ksz, rsz, psz = g(8), g(16), g(36)
    al = lambda x: (x + psz - 1)//psz*psz
    return dict(magic=hex(g(0)), ksz=ksz, kaddr=hex(g(12)), rsz=rsz, raddr=hex(g(20)),
                ssz=g(24), saddr=hex(g(28)), taddr=hex(g(32)), psz=psz,
                hver=g(40), osver=hex(g(44)),
                name=d[48:64].split(b'\0',1)[0].decode(),
                cmdline=d[64:576].split(b'\0',1)[0].decode(),
                koff=psz, roff=psz+al(ksz), soff=psz+al(ksz)+al(rsz))
a = geom(sys.argv[1]); b = geom(sys.argv[2])
print()
print("  %-9s %-30s %-30s %s" % ("field", "iteration 2", "iteration 5", ""))
for n in ['magic','hver','osver','psz','kaddr','raddr','saddr','taddr','ksz','rsz','ssz','name','koff','roff','soff']:
    mark = "" if a[n] == b[n] else "  <-- DIFFERS"
    print("  %-9s %-30s %-30s%s" % (n, a[n], b[n], mark))
print("  %-9s len=%-27d len=%-27d %s" % ("cmdline", len(b['cmdline']), len(a['cmdline']),
                                         "SAME" if a['cmdline']==b['cmdline'] else "<-- DIFFERS"))
print("  %-9s %-30s %-30s %s" % ("cmdline==", "", "", "identical" if a['cmdline']==b['cmdline'] else "DIFFERENT"))
if a['cmdline'] != b['cmdline']:
    print("    iter2:", b['cmdline'])
    print("    iter5:", a['cmdline'])
print()
for tag, d in (("iter2", b), ("iter5", a)):
    f = open(sys.argv[1] if tag=="iter5" else sys.argv[2], 'rb')
    f.seek(d['roff']); r = f.read(d['rsz'])
    print("  %s ramdisk sha256: %s (%d bytes)" % (tag, hashlib.sha256(r).hexdigest(), d['rsz']))
    f.seek(d['koff']); k = f.read(d['ksz'])
    print("  %s kernel  sha256: %s (%d bytes)" % (tag, hashlib.sha256(k).hexdigest(), d['ksz']))
print("  file size: %d" % len(open(sys.argv[1],'rb').read()))
PY

hdr "11. byte-level diff, iteration 2 vs iteration 5"
python3 - "$ITER2" "$NEW_IMG" <<'PY'
import sys
a = open(sys.argv[1],'rb').read(); b = open(sys.argv[2],'rb').read()
print("  sizes: iter2=%d iter5=%d (delta %+d)" % (len(a), len(b), len(b)-len(a)))
n = min(len(a), len(b)); runs=[]; i=0
while i < n:
    if a[i] != b[i]:
        j=i
        while j < n and a[j] != b[j]: j+=1
        runs.append((i,j)); i=j
    else: i+=1
tot=sum(j-i for i,j in runs)
print("  differing bytes: %d in %d runs" % (tot, len(runs)))
for i,j in runs[:8]:
    print("    0x%08x-0x%08x (%d B)" % (i,j,j-i))
if len(runs) > 8: print("    ... and %d more runs" % (len(runs)-8))
print("  largest runs:")
for i,j in sorted(runs, key=lambda r: r[0]-r[1])[:6]:
    print("    0x%08x-0x%08x (%d B)" % (i,j,j-i))
PY

hdr "RESULT"
[ $RC -eq 0 ] && echo "  all checks passed" || echo "  one or more checks FAILED"
exit $RC
