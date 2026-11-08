# -*- coding: utf-8 -*-
# chiTOF: dump the variant-1 pipeline descriptor (0x284fbc) + its stream list.
# Pipeline list base = descriptor(0x284f54)+0x14 -> 0x284f78; variant1 = +0x44.
# @category: RE
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_pipe1.txt"
out = open(OUT, "w")
program = currentProgram
mem = program.getMemory()
asm = program.getAddressFactory().getDefaultAddressSpace()

def rd32(off):
    try:
        return mem.getInt(asm.getAddress(off)) & 0xFFFFFFFF
    except Exception:
        return None

def name_at(off):
    bs = b""
    a = asm.getAddress(off)
    try:
        for _ in range(48):
            b = mem.getByte(a) & 0xFF
            if b == 0: break
            bs += bytes([b]); a = a.add(1)
        s = bs.decode("ascii", "replace")
        return s if s and all(31 < ord(c) < 127 for c in s) else None
    except Exception:
        return None

FMT = {0x20:"RAW16",0x21:"BLOB",0x22:"IMPL_DEF",0x23:"YUV420",0x2b:"RAW10",0x25:"RAW12",0x20363159:"Y16",0x24:"RAW_OPAQUE"}

def dump(base, span, label):
    out.write("\n== %s @0x%x ==\n" % (label, base))
    for i in range(0, span, 4):
        v = rd32(base + i)
        if v is None: continue
        note = ""
        if 0x20000 <= v <= 0x310000:
            s = name_at(v)
            if s: note = "  str %r" % s
        if v in FMT: note += "  FMT=%s" % FMT[v]
        if v in (640,480,320,240,1280,3840,2560,1920,0x1000,4915200,9830400): note += "  DIM=%d" % v
        out.write("  +0x%02x: 0x%08x %-11d%s\n" % (i, v, v, note))

# variant1 pipeline descriptor
dump(0x284fbc, 0x44, "pipeline[1] desc")
sl = rd32(0x284fbc + 0x38)
cnt = rd32(0x284fbc + 0x3c)
out.write("\nvariant1 streamList=0x%x count=%s\n" % (sl, cnt))
if sl:
    dump(sl, 0x120, "pipeline[1] streamList")
extra = rd32(0x284fbc + 0x40)
if extra:
    dump(extra, 0x120, "pipeline[1] extraList(+0x40)")

# also dump all 4 variant pipeline descriptors' names + streamlist ptrs
out.write("\n== all 4 variants ==\n")
for v in range(4):
    pd = 0x284f78 + v * 0x44
    nm = rd32(pd + 0x04)
    nms = name_at(nm) if nm else None
    out.write("variant %d @0x%x: name_ptr=0x%x %r  streamlist(+0x38)=0x%x count(+0x3c)=%s\n" % (
        v, pd, nm if nm else 0, nms if False else nm and name_at(nm), rd32(pd+0x38), rd32(pd+0x3c)))

out.close()
print("wrote", OUT)
