# -*- coding: utf-8 -*-
# 2-hop string hunt: code -> literal pool -> string, for [SS_PARAM] and video matching strings
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_ssparam2.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refmgr = program.getReferenceManager()
di = DecompInterface()
di.openProgram(program)

targets = ["[SS_PARAM][Shooting Mode]", "shootingMode tag", "[SS_PARAM][SAMSUNG]",
           "Set Custom Usecase", "Default sensor FPS", "SecCamVideoUsecase Initialize failed",
           "Matching Usecase id = SecCamVideo", "Invalid Format"]
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
    # hop 1: references to this string (from literal pools or code)
    for r1 in refmgr.getReferencesTo(addr):
        frm = r1.getFromAddress()
        # hop 2: who references the literal pool slot (if frm is data)?
        f = fm.getFunctionContaining(frm)
        if f is not None:
            hits.setdefault(f.getEntryPoint(), f)
            out.write("STR %r <- %s (in %s @ %s)\n" % (s[:60], frm, f.getName(), f.getEntryPoint()))
            continue
        for r2 in refmgr.getReferencesTo(frm):
            f2 = fm.getFunctionContaining(r2.getFromAddress())
            if f2 is not None:
                hits.setdefault(f2.getEntryPoint(), f2)
                out.write("STR %r <- %s <- %s (in %s @ %s)\n" % (s[:60], frm, r2.getFromAddress(), f2.getName(), f2.getEntryPoint()))

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
