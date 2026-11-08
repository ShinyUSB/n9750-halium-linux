# -*- coding: utf-8 -*-
# Decompile sehPreProcessConfigurationLocked + convertFromHidl by name search
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/sehstream_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

count = 0
for f in fm.getFunctions(True):
    name = f.getName()
    if any(k in name for k in ("PreProcessConfiguration", "convertFromHidl", "PostProcessConfiguration")):
        res = di.decompileFunction(f, 180, monitor)
        out.write("\n//================ %s @ %s size=%d\n" % (name, f.getEntryPoint(), f.getBody().getNumAddresses()))
        if res.decompileCompleted():
            out.write(res.getDecompiledFunction().getC())
        else:
            out.write("// fail: %s\n" % res.getErrorMessage())
        count += 1
        if count > 6:
            break
out.close()
print("DONE %d -> " % count + OUT)
