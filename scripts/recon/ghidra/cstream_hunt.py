# -*- coding: utf-8 -*-
# Decompile CameraStream methods
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/cstream.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

for f in fm.getFunctions(True):
    name = f.getName()
    if "CameraStream" in name or "GetScenarioType" in name:
        if "Thunk" in name:
            continue
        res = di.decompileFunction(f, 120, monitor)
        if res.decompileCompleted():
            out.write("\n//================ %s @ %s size=%d\n" % (name, f.getEntryPoint(), f.getBody().getNumAddresses()))
            out.write(res.getDecompiledFunction().getC())

out.close()
print("DONE -> " + OUT)
