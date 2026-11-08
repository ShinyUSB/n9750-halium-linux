# -*- coding: utf-8 -*-
# Find the SecCamTOF match condition: decompile the usecase-selection paths that
# reference the SecCamTOF factory/usecase (via the usecase name string table).
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_match.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

def decomp(fn):
    if fn is None:
        return "(null)"
    res = di.decompileFunction(fn, 180, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()
    return "(decomp failed)"

# find functions that reference any string containing "SecCamTOF" (the class name
# is also the usecase name — the matcher's switch selects on it)
for d in listing.getDefinedData(True):
    try:
        val = d.getValue()
    except Exception:
        continue
    sval = None
    try:
        sval = str(val)
    except Exception:
        continue
    if sval and "SecCamTOF" in sval and len(sval) < 60:
        out.write("=== string %r at %s ===\n" % (sval, d.getMinAddress()))
        for r in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            fn = fm.getFunctionContaining(r.getFromAddress())
            if fn:
                out.write("  from %s (%s)\n" % (fn.getName(), r.getFromAddress()))
                out.write(decomp(fn)[:14000])
                out.write("\n")
out.close()
print("wrote", OUT)
