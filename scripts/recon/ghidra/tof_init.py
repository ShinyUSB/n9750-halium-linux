# -*- coding: utf-8 -*-
# Find all functions referencing chxseccamtofusecase.cpp and decompile the
# Initialize/Create/ConfigureStreams ones.
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_init.txt"
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

# the source path string + all refs to it
srcstr = "chxseccamtofusecase.cpp"
found = set()
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
    if sval and srcstr in sval:
        out.write("src %s at %s\n" % (sval[:40], d.getMinAddress()))
        for r in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
            fn = fm.getFunctionContaining(r.getFromAddress())
            if fn:
                found = True
                nm = fn.getName()
                out.write("  fn %s (%s)\n" % (nm, fn.getEntryPoint()))
                # decompile the interesting ones
                if re.search(r'[Ii]nit|[Cc]reate|[Cc]onfigure|[Mm]atch|[Cc]heck|Open|[Ss]tream', nm):
                    out.write(decomp(fn)[:14000])
                    out.write("\n")

# ALSO: find SecCamTOFUsecase::Create (the factory) by following the
# usecase-selector's table. The class's vtable references the destructor
# at 001cb930; the vtable's other slots = the class methods. Find the vtable:
for fn in fm.getFunctions(True):
    if "SecCamTOFUsecase" in fn.getName():
        ep = fn.getEntryPoint()
        out.write("func %s at %s\n" % (fn.getName(), ep))
out.close()
print("wrote", OUT, len(found))
