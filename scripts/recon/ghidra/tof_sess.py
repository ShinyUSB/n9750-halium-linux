# -*- coding: utf-8 -*-
# Decompile functions referencing the ToF session / depth-stream strings.
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_sess.txt"
KEYS = ["YuvUniToTofSession", "Depth16 Stream", "DepthConfidence", "m_pTofPreviewStream",
        "SecCamTofRawBufferManager", "realtime ToF session", "uni tof session",
        "Intenal TOF YUV", "rear_tof_fps", "front_tof_fps", "TOF_DBG", "tofBokehValue",
        "samsung.android.depth", "depth16Size", "isProbeTOFSensor", "availableDepthStream"]
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

seen = set()
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
    if not sval or len(sval) > 220:
        continue
    if any(k in sval for k in KEYS):
        for r in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            fn = fm.getFunctionContaining(r.getFromAddress())
            if fn and fn.getName() not in seen:
                seen.add(fn.getName())
                out.write("\n===== %s (%s)  <<%s>> =====\n" % (fn.getName(), fn.getEntryPoint(), sval[:80]))
                out.write(decomp(fn)[:12000])
out.close()
print("wrote", OUT, len(seen), "functions")
