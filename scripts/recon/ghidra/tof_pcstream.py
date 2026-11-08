# -*- coding: utf-8 -*-
# chiTOF (com.samsung.chi.override.so): resolve the CHIBufferManager allocator
# pointers used by FeatureTOF::OverrideUsecase and find TOFPointCloud stream setup.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_pcstream.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
mem = program.getMemory()
di = DecompInterface()
di.openProgram(program)
asm = program.getAddressFactory().getDefaultAddressSpace()

def decomp(fn, cap=14000):
    if fn is None:
        return "(null fn)"
    res = di.decompileFunction(fn, 240, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

def ptr_name(addr):
    try:
        v = mem.getInt(addr)
    except Exception as e:
        return "read fail %s" % e
    tgt = asm.getAddress(v & 0xFFFFFFFF)
    fn = fm.getFunctionAt(tgt)
    if fn:
        return "0x%x -> %s (%s)" % (v & 0xFFFFFFFF, fn.getName(), tgt)
    sym = program.getSymbolTable().getPrimarySymbol(tgt)
    return "0x%x -> %s" % (v & 0xFFFFFFFF, sym if sym else tgt)

# 1. allocator fn ptr table at 0x1d2210 / 0x1d2218 (as u32 ptrs, PIC-adjusted values
#    stored as DAT_ constants; read raw)
out.write("== allocator table at 0x1d2210..0x1d2228 ==\n")
for off in range(0x1d2210, 0x1d2230, 4):
    out.write("0x%x: %s\n" % (off, ptr_name(asm.getAddress(off))))

# 2. find TOFPointCloud strings + xref functions
out.write("\n== TOFPointCloud strings ==\n")
seen = set()
for d in listing.getDefinedData(True):
    try:
        sval = str(d.getValue())
    except Exception:
        continue
    if sval and "TOFPointCloud" in sval:
        out.write("str @%s: %r\n" % (d.getMinAddress(), sval[:140]))
        for ref in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            f = fm.getFunctionContaining(ref.getFromAddress())
            if f and f.getEntryPoint().getOffset() not in seen:
                seen.add(f.getEntryPoint().getOffset())
                out.write("\n===== %s (%s) =====\n" % (f.getName(), f.getEntryPoint()))
                out.write(decomp(f))

# 3. find "PointCloud" partials too
out.write("\n== PointCloud strings ==\n")
for d in listing.getDefinedData(True):
    try:
        sval = str(d.getValue())
    except Exception:
        continue
    if sval and "ointCloud" in sval and "TOFPointCloud" not in sval:
        out.write("str @%s: %r\n" % (d.getMinAddress(), sval[:140]))
        for ref in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            f = fm.getFunctionContaining(ref.getFromAddress())
            if f and f.getEntryPoint().getOffset() not in seen:
                seen.add(f.getEntryPoint().getOffset())
                out.write("\n===== %s (%s) =====\n" % (f.getName(), f.getEntryPoint()))
                out.write(decomp(f))

out.close()
print("wrote", OUT, "funcs", len(seen))
