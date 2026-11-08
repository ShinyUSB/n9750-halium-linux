# -*- coding: utf-8 -*-
# Decompile VendorHalLoader and MultiCameraManager functions
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/vendor_loader_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

count = 0
for f in fm.getFunctions(True):
    name = f.getName()
    if any(t in name for t in ("VendorHalLoader", "MultiCameraManager", "GetVendorHalModule",
                                 "AdjustPublicModuleOps", "hw_get_module", "LoadVendorHal")):
        res = di.decompileFunction(f, 120, monitor)
        if res.decompileCompleted():
            out.write("\n//================ %s @ %s size=%d\n" % (name, f.getEntryPoint(), f.getBody().getNumAddresses()))
            out.write(res.getDecompiledFunction().getC())
            count += 1
            if count > 12:
                break
out.close()
print("DONE %d -> " % count + OUT)
