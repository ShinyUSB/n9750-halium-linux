# -*- coding: utf-8 -*-
# Ghidra headless post-script: find + decompile the SecCamTOF usecase trigger.
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_hunt.txt"
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
    res = di.decompileFunction(fn, 120, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()
    return "(decomp failed)"

# 1. find all defined strings referencing SecCamTOF / tof / depth / usecase
hits = {}
KEYS = ["SecCamTOF", "TOF", "ToF", "tof", "SecCamTof", "depth", "Depth", "SecCam", "Matching Usecase"]
for d in listing.getDefinedData(True):
    try:
        val = d.getValue()
    except Exception:
        continue
    if not isinstance(val, (str,)) and not isinstance(val, basestring if False else str):
        pass
    sval = None
    try:
        sval = str(val)
    except Exception:
        continue
    if not sval or len(sval) > 200 or len(sval) < 3:
        continue
    if any(k in sval for k in KEYS):
        for r in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            frm = r.getFromAddress()
            fn = fm.getFunctionContaining(frm)
            if fn:
                hits.setdefault(fn.getName(), set()).add(sval)
out.write("== functions referencing tof/depth/usecase strings ==\n")
for fn, strs in sorted(hits.items()):
    out.write("%s <- %s\n" % (fn, sorted(list(strs))[:4]))

# 2. decompile the key functions
for want in ["chxseccamtofusecase", "GetMatchingUsecase", "MatchingUsecase", "UsecaseSelector",
             "getSensorDimension", "initializeTOF", "createTOF", "UsecaseDepth", "SecCamTOF"]:
    for fn in fm.getFunctions(True):
        if want.lower() in fn.getName().lower():
            out.write("\n===== %s (%s) =====\n" % (fn.getName(), fn.getEntryPoint()))
            out.write(decomp(fn)[:14000])
            out.write("\n")
out.close()
print("wrote", OUT, "hits", len(hits))
