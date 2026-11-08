# -*- coding: utf-8 -*-
# Decompile CameraHalUnit::ConfigureStreams and the bypass path in unihal_main
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/unihal_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

# find functions with these names or containing key strings
targets = ["ConfigureStreams", "bypass", "Bypass"]
count = 0
for f in fm.getFunctions(True):
    name = f.getName()
    sz = f.getBody().getNumAddresses()
    if any(t in name for t in ("ConfigureStreams", "UniState", "ScenarioManager", "CameraHalUnit")):
        if count > 20:
            break
        res = di.decompileFunction(f, 120, monitor)
        if res.decompileCompleted():
            c = res.getDecompiledFunction().getC()
            if any(k in c for k in ("configure", "stream", "bypass", "Bypass", "0x41", "0x10000")):
                out.write("\n//================ %s @ %s size=%d\n" % (name, f.getEntryPoint(), sz))
                out.write(c)
                count += 1
out.close()
print("DONE %d -> " % count + OUT)
