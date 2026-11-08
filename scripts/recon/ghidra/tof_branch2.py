# -*- coding: utf-8 -*-
# Disassemble the GetMatchingUsecase region around the SecCamTOF log reference.
# @category: RE
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_branch2.txt"
out = open(OUT, "w")
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refm = program.getReferenceManager()

# locate the string
target = None
for d in listing.getDefinedData(True):
    try:
        sval = str(d.getValue())
    except Exception:
        continue
    if sval and "SecCamTOF" in sval and len(sval) < 60:
        out.write("SecCamTOF string %r at %s\n" % (sval, d.getMinAddress()))
        for r in refm.getReferencesTo(d.getMinAddress()):
            fa = r.getFromAddress()
            fn = fm.getFunctionContaining(fa)
            if fn:
                out.write("\n=== %s (%s), ref at %s ===\n" % (fn.getName(), fn.getEntryPoint(), fa))
                body = fn.getBody()
                it = listing.getInstructions(body, True)
                instrs = []
                while it.hasNext():
                    ins = it.next()
                    instrs.append((ins.getAddress(), str(ins)))
                idx = None
                for k, (a, t) in enumerate(instrs):
                    if a == fa:
                        idx = k; break
                if idx is None:
                    idx = len(instrs) - 1
                lo = max(0, idx - 60); hi = min(len(instrs), idx + 6)
                for a, t in instrs[lo:hi]:
                    mark = " <<<" if a == fa else ""
                    out.write("  %s: %s%s\n" % (a, t, mark))
out.close()
print("wrote", OUT)
