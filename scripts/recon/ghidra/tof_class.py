# -*- coding: utf-8 -*-
# Decompile every function referencing chxseccamtofusecase.cpp (the SecCamTOF class).
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_class.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

def decomp(fn):
    res = di.decompileFunction(fn, 180, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()
    return "(decomp failed)"

seen = set()
for d in listing.getDefinedData(True):
    try:
        sval = str(d.getValue())
    except Exception:
        continue
    if "chxseccamtofusecase" in sval:
        out.write("src %s at %s\n" % (sval[:60], d.getMinAddress()))
        for r in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            fn = fm.getFunctionContaining(r.getFromAddress())
            if fn:
                key = fn.getName()
                if key not in seen:
                    seen.add(key)
                    out.write("\n===== %s (%s) =====\n" % (key, fn.getEntryPoint()))
                    out.write(decomp(fn)[:13000])
out.write("\n=== SecCamTOF class functions: %d ===\n" % len(seen))
for f in sorted(seen):
    out.write("  %s\n" % f)
out.close()
print("wrote", OUT, len(seen))
