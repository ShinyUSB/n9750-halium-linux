# -*- coding: utf-8 -*-
# chiTOF: dump the 3 pipeline descriptor structs of UsecaseTOFRaw16.
# @category: RE
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_pipes.txt"
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
            if b == 0:
                break
            bs += bytes([b])
            a = a.add(1)
        s = bs.decode("ascii", "replace")
        return s if s and all(31 < ord(c) < 127 for c in s) else None
    except Exception:
        return None

def dump(base, span, label):
    out.write("\n== %s @0x%x ==\n" % (label, base))
    for i in range(0, span, 4):
        v = rd32(base + i)
        if v is None:
            continue
        note = ""
        if 0x20000 <= v <= 0x310000:
            s = name_at(v)
            if s:
                note = "  str %r" % s
        # annotate interesting constants
        if v in (0x20, 0x21, 0x22, 0x23, 0x20363159):
            note += "  <fmt>"
        if v in (640, 480, 320, 240, 1280, 3840, 0x1000, 4915200, 4915236, 4915248, 9830400):
            note += "  <%d>" % v
        out.write("  +0x%03x: 0x%08x %d%s\n" % (i, v, v, note))

# descriptor says pipelines at 0x285088, 0x2850a8, 0x2850c8 ; and list head 0x284f6c/0x284f78
for base in (0x284f6c, 0x284f78, 0x285088, 0x2850a8, 0x2850c8):
    dump(base, 0x120, "pipe")

out.close()
print("wrote", OUT)
