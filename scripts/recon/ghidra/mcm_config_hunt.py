# -*- coding: utf-8 -*-
# Decompile MultiCameraManager::ConfigureStreams specifically
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/mcm_configure.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

for f in fm.getFunctions(True):
    name = f.getName()
    if "ConfigureStreams" in name and "Thunk" not in name:
        sz = f.getBody().getNumAddresses()
        out.write("\n//================ %s @ %s size=%d\n" % (name, f.getEntryPoint(), sz))
        res = di.decompileFunction(f, 180, monitor)
        if res.decompileCompleted():
            out.write(res.getDecompiledFunction().getC())
        else:
            out.write("// fail: %s\n" % res.getErrorMessage())

out.close()
print("DONE -> " + OUT)
