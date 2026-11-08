# -*- coding: utf-8 -*-
# chiTOF: dump the SecCamTOFUsecase descriptor + pipeline stream tables.
# The usecase descriptor ptr lands in this+0x9c78 = DAT_001ccb14 + 0x1cc1e0.
# Also find the data struct that references the 'TOFPointCloudOut1' name string.
# @category: RE
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_desc.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
mem = program.getMemory()
listing = program.getListing()
asm = program.getAddressFactory().getDefaultAddressSpace()

def rd32(off):
    try:
        return mem.getInt(asm.getAddress(off)) & 0xFFFFFFFF
    except Exception as e:
        return None

def name_at(off):
    # read a C string at file vaddr
    bs = b""
    a = asm.getAddress(off)
    for _ in range(64):
        b = mem.getByte(a) & 0xFF
        if b == 0:
            break
        bs += bytes([b])
        a = a.add(1)
    try:
        return bs.decode("ascii", "replace")
    except Exception:
        return repr(bs)

# 1. the descriptor base via the PIC constant: value stored at 0x1ccb14
v = rd32(0x1ccb14)
out.write("DAT_001ccb14 value = 0x%x ; descriptor candidate base = 0x%x\n" % (v if v else -1, ((v or 0) + 0x1cc1e0) & 0xFFFFFFFF))
desc = ((v or 0) + 0x1cc1e0) & 0xFFFFFFFF

# dump a chunk of the descriptor: it starts with a name ptr, then counts, then pipeline desc ptrs
out.write("\n== descriptor @0x%x raw dump ==\n" % desc)
for i in range(0, 0x40, 4):
    val = rd32(desc + i)
    note = ""
    if val and 0x20000 <= val <= 0x300000:
        s = name_at(val)
        if s and all(32 <= ord(c) < 127 for c in s[:4]):
            note = "  -> str %r" % s
    out.write("+0x%02x: 0x%08x%s\n" % (i, val if val is not None else -1, note))

# 2. find data references to the TOFPointCloudOut1 string (0x28351)
out.write("\n== refs to 'TOFPointCloudOut1' (0x28351) ==\n")
target = asm.getAddress(0x28351)
for ref in program.getReferenceManager().getReferencesTo(target):
    out.write("ref from %s\n" % ref.getFromAddress())

# 3. scan data for the stream-name strings of the tof pipeline (they cluster)
for s in ["TOFPointCloudOut1", "TOFPointCloudOut2", "Depth16", "TofPreview", "DepthConfidence"]:
    for d in listing.getDefinedData(True):
        try:
            sv = str(d.getValue())
        except Exception:
            continue
        if sv and s in sv:
            out.write("str %-18s @%s: %r\n" % (s, d.getMinAddress(), sv[:80]))

out.close()
print("wrote", OUT)
