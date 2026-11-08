# -*- coding: utf-8 -*-
# Decompile functions that reference the SecCamTOF depth-stream creation strings
# (the usecase's Initialize/pipeline setup).
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_init2.txt"
KEYS = ["TOFPointCloudOut", "Depth16 Stream is coming", "m_pDepth16Stream", "TofPreviewStream",
        "FeatureTof Session", "Depth16Size", "SecCamTofDepth16BufferManager",
        "Received call for TOF Depth frame", "Depth Stream is setted", "Confidence Stream"]
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
    if sval and any(k in sval for k in KEYS) and len(sval) < 200:
        for r in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            fn = fm.getFunctionContaining(r.getFromAddress())
            if fn and fn.getName() not in seen:
                seen.add(fn.getName())
                out.write("\n===== %s (%s)  <<%s>> =====\n" % (fn.getName(), fn.getEntryPoint(), sval[:70]))
                out.write(decomp(fn)[:13000])
out.close()
print("wrote", OUT, len(seen))
