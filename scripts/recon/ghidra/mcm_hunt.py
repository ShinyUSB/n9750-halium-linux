# -*- coding: utf-8 -*-
# Decompile MultiCameraManager::OpenCamera and Initialize
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/mcm_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

for f in fm.getFunctions(True):
    name = f.getName()
    if any(t in name for t in ("OpenCamera", "Initialize", "GetNumberOfCamera",
                                 "GetInstance", "CloseCamera")):
        if "Thunk" in name:
            continue
        res = di.decompileFunction(f, 120, monitor)
        if res.decompileCompleted():
            out.write("\n//================ %s @ %s size=%d\n" % (name, f.getEntryPoint(), f.getBody().getNumAddresses()))
            out.write(res.getDecompiledFunction().getC())
out.close()
print("DONE -> " + OUT)
