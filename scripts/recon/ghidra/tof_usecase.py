# -*- coding: utf-8 -*-
# Decompile all SecCamTOFUsecase + SecFeatureTOF functions + the usecase registration.
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_usecase.txt"
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

# find functions whose name mentions the TOF usecase or feature
seen = set()
for fn in fm.getFunctions(True):
    nm = fn.getName()
    if re.search(r'SecCamTOF|SecCamTof|SecFeatureTOF|SecFeatureTof|FeatureTOF|FeatureTof', nm, re.IGNORECASE):
        key = nm
        if key in seen:
            continue
        seen.add(key)
        out.write("\n===== %s (%s) =====\n" % (nm, fn.getEntryPoint()))
        out.write(decomp(fn)[:16000])
        out.write("\n")

# also: find functions referencing the "Matching Usecase id = SecCamTOF" string
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
    if sval and "SecCamTOF" in sval:
        out.write("\n=== string refs to %r at %s ===\n" % (sval, d.getMinAddress()))
        for r in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            fn = fm.getFunctionContaining(r.getFromAddress())
            out.write("  from %s (%s)\n" % (fn.getName() if fn else "?", r.getFromAddress()))
            if fn:
                out.write(decomp(fn)[:9000])
out.close()
print("wrote", OUT, len(seen), "functions")
