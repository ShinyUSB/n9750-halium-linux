# -*- coding: utf-8 -*-
# Find functions referencing shooting-mode / session-param strings, decompile them
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_shootingparam.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refmgr = program.getReferenceManager()
di = DecompInterface()
di.openProgram(program)

targets = ["[SS_PARAM][Shooting Mode]", "shootingMode tag", "[SS_PARAM][SAMSUNG]",
           "[SS_PARAM][Ssm ShotMode]", "SetCaptureStatus", "UseCaseSelector"]
hits = {}

for d in listing.getDefinedData(True):
    try:
        v = d.getValue()
    except Exception:
        continue
    if not (isinstance(v, str) or isinstance(v, unicode)):
        continue
    s = str(v)
    for t in targets:
        if t in s:
            addr = d.getAddress()
            for r in refmgr.getReferencesTo(addr):
                f = fm.getFunctionContaining(r.getFromAddress())
                if f is not None and "Thunk" not in f.getName():
                    hits[f.getEntryPoint()] = f
            break

out.write("hit functions: %d\n" % len(hits))
for ep, f in sorted(hits.items()):
    out.write("\n//================ %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
    res = di.decompileFunction(f, 180, monitor)
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
    else:
        out.write("// decompile failed\n")

out.close()
print("DONE -> " + OUT)
