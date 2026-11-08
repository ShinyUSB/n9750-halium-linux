# -*- coding: utf-8 -*-
# Decompile MultiCameraManager::OpenCamera and all functions it calls
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/mcm_open.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

# Find MultiCameraManager::OpenCamera by searching for the symbol
target = None
for f in fm.getFunctions(True):
    if "OpenCamera" in f.getName() and "Thunk" not in f.getName():
        target = f
        out.write("found: %s @ %s\n" % (f.getName(), f.getEntryPoint()))
        break

if target:
    res = di.decompileFunction(target, 180, monitor)
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
    # also decompile all functions it calls (callees)
    callees = set()
    for ref in target.getCalledFunctions(monitor):
        callees.add(ref)
    out.write("\n\n//=== CALLEES (%d) ===\n" % len(callees))
    for cf in sorted(callees, key=lambda x: x.getName()):
        if "Thunk" in cf.getName():
            continue
        res2 = di.decompileFunction(cf, 90, monitor)
        if res2.decompileCompleted():
            c = res2.getDecompiledFunction().getC()
            if len(c) < 8000:  # skip huge ones
                out.write("\n//================ %s @ %s\n" % (cf.getName(), cf.getEntryPoint()))
                out.write(c)
            else:
                out.write("\n// %s (skipped, %d bytes)\n" % (cf.getName(), len(c)))
else:
    out.write("OpenCamera not found!\n")

# Also find and decompile the hw_module_methods open wrapper in camera.unihal.default.so
out.write("\n\n//=== Also: search for 'open' in unihal_main that wraps hw open ===\n")
for f in fm.getFunctions(True):
    if "open" in f.getName().lower() and "Thunk" not in f.getName():
        sz = f.getBody().getNumAddresses()
        if sz > 100 and sz < 600:
            res3 = di.decompileFunction(f, 60, monitor)
            if res3.decompileCompleted():
                c = res3.getDecompiledFunction().getC()
                if "hw_device" in c or "hw_module" in c or "0x80" in c:
                    out.write("\n//================ %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), sz))
                    out.write(c)

out.close()
print("DONE -> " + OUT)
