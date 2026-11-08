# -*- coding: utf-8 -*-
# chiTOF: dump the stream-list tables referenced by UsecaseTOFRaw16 pipelines.
# @category: RE
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_streams.txt"
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

INTEREST = {0x20:"RAW16?",0x21:"BLOB",0x22:"IMPL_DEF",0x23:"YUV",0x20363159:"Y16",
            640:"",480:"",320:"",240:"",1280:"",3840:"",2560:"",1920:"",
            0x1000:"ds0x1000",4915200:"",4915236:"",9830400:""}

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
        if v in INTEREST:
            note += "  *0x%x/%d%s" % (v, v, INTEREST[v])
        out.write("  +0x%03x: 0x%08x %-12d%s\n" % (i, v, v, note))

for base in (0x2850e8, 0x2850f4, 0x285154, 0x285234, 0x28524c, 0x2852ac,
             0x28538c, 0x2853a4, 0x285404, 0x28551c, 0x28554c, 0x2855ac):
    dump(base, 0x70, "streamtbl")

out.close()
print("wrote", OUT)
