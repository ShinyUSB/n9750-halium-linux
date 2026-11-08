# -*- coding: utf-8 -*-
# Find + decompile the CSL buffer-map failure chain in camera32.qcom.so:
#   ImportGrallocBuffer -> CSLMapNativeBuffer -> CSLMapBufferHW -> CSLHwAddBuffer
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/csl_map_chain.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

TARGETS = [
    "CSLHwAddBuffer failed",
    "CSLMapNativeBuffer failed",
    "Failed in importing Gralloc Buffer",
    "invalid type streamConfig",
    "Invalid BindImportedBuffer request",
]

def find_strings(s):
    res = []
    for d in listing.getDefinedData(True):
        try:
            sval = str(d.getValue())
        except Exception:
            continue
        if sval and s in sval and len(sval) < 260:
            res.append((d.getMinAddress(), sval))
    return res

def decomp(fn, cap=16000):
    res = di.decompileFunction(fn, 240, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

seen = set()
for t in TARGETS:
    hits = find_strings(t)
    out.write("\n########## string %r -> %d hit(s)\n" % (t, len(hits)))
    for addr, val in hits:
        out.write("# str at %s: %r\n" % (addr, val[:120]))
        for ref in program.getReferenceManager().getReferencesTo(addr):
            f = fm.getFunctionContaining(ref.getFromAddress())
            if f and f.getEntryPoint().getOffset() not in seen:
                seen.add(f.getEntryPoint().getOffset())
                out.write("\n===== %s (%s)  <<%s>> =====\n" % (f.getName(), f.getEntryPoint(), t))
                out.write(decomp(f))
            elif not f:
                out.write("ref from %s (no function)\n" % ref.getFromAddress())

out.close()
print("wrote", OUT, "funcs:", len(seen))
