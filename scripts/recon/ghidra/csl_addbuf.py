# -*- coding: utf-8 -*-
# Decompile CSLHwAddBuffer + CSLMapNativeBuffer + related by symbol name in camera32.qcom.so
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/csl_addbuf.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

WANT = ["CSLHwAddBuffer", "CSLMapNativeBuffer", "CSLHwPopulateMMUHandles",
        "CSLHwPopulateCDMMMUHandles", "CSLHwLookupBuffer", "CSLHwGetDeviceFD",
        "CSLUnmapBufferHW", "CSLHwRemoveBuffer"]

def decomp(fn, cap=20000):
    res = di.decompileFunction(fn, 240, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

done = set()
for want in ["CSLHwAddBuffer", "CSLMapNativeBuffer", "CSLHwPopulateMMUHandles",
             "CSLHwPopulateCDMMMUHandles", "CSLHwLookupBuffer"]:
    for fn in fm.getFunctions(True):
        nm = fn.getName()
        if want in nm and fn.getEntryPoint().getOffset() not in done:
            done.add(fn.getEntryPoint().getOffset())
            out.write("\n===== %s (%s) =====\n" % (nm, fn.getEntryPoint()))
            out.write(decomp(fn))
            out.write("\n")
out.close()
print("wrote", OUT, len(done))
