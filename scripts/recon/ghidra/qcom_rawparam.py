# -*- coding: utf-8 -*-
# Find the rawParam parser in camera.qcom.so: refs to "samsungcamera" / "recording-fps" strings
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/qcom_rawparam.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refmgr = program.getReferenceManager()
di = DecompInterface()
di.openProgram(program)

targets = ["samsungcamera", "recording-fps", "shootingmode", "first-entrance", "factorytest",
           "operation_mode", "Invalid streamStype", "No proper usecase"]
hits = {}
for d in listing.getDefinedData(True):
    try:
        v = d.getValue()
    except Exception:
        continue
    if not (isinstance(v, str) or isinstance(v, unicode)):
        continue
    s = str(v)
    matched = None
    for t in targets:
        if t in s:
            matched = t
            break
    if matched is None:
        continue
    addr = d.getAddress()
    out.write("STRING %r @ %s\n" % (s[:70], addr))
    for r1 in refmgr.getReferencesTo(addr):
        frm = r1.getFromAddress()
        f = fm.getFunctionContaining(frm)
        if f is not None:
            hits.setdefault(f.getEntryPoint(), f)
            out.write("   direct ref from %s @ %s\n" % (f.getName(), f.getEntryPoint()))
            continue
        for r2 in refmgr.getReferencesTo(frm):
            f2 = fm.getFunctionContaining(r2.getFromAddress())
            if f2 is not None:
                hits.setdefault(f2.getEntryPoint(), f2)
                out.write("   2-hop ref from %s @ %s\n" % (f2.getName(), f2.getEntryPoint()))

out.write("\nhit functions: %d\n\n" % len(hits))
for ep, f in sorted(hits.items()):
    out.write("\n//================ %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
    res = di.decompileFunction(f, 240, monitor)
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
    else:
        out.write("// decompile failed\n")

out.close()
print("DONE -> " + OUT)
