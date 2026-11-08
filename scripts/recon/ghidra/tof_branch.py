# -*- coding: utf-8 -*-
# Disassemble GetMatchingUsecase + find the SecCamTOF selection condition.
# Print: (a) all call sites to SecCamTOFUsecase methods, (b) the branch right before
# the "Matching Usecase id = SecCamTOF" log call, with the compared registers/values.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.listing import Instruction

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_branch.txt"
out = open(OUT, "w")
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refm = program.getReferenceManager()

# 1. the SecCamTOF log string
for d in listing.getDefinedData(True):
    try:
        val = d.getValue()
    except Exception:
        continue
    sval = None
    try:
        sval = str(val)
    except Exception:
        continue
    if sval and sval.strip() == "Matching Usecase id = SecCamTOF":
        out.write("string at %s\n" % d.getMinAddress())
        for r in ref_all(d, ref_all):
            pass

def refs_to(addr):
    return ref_all(addr)

def ref_all(addr):
    return []

for d in listing.getDefinedData(True):
    try:
        sval = str(d.getValue())
    except Exception:
        continue
    if sval and "SecCamTOF" in sval and len(sval) < 60:
        out.write("SecCamTOF string %r at %s\n" % (sval, d.getMinAddress()))
        for r in refm.getReferencesTo(d.getMinAddress()):
            out.write("  ref from %s\n" % r.getFromAddress())

# disassemble around each ref site
for d in listing.getDefinedData(True):
    try:
        sval = str(d.getValue())
    except Exception:
        continue
    if sval and "SecCamTOF" in sval and len(sval) < 60:
        for r in refm.getReferencesTo(d.getMinAddress()):
            fa = r.getFromAddress()
            fn = fm.getFunctionContaining(fa)
            if fn:
                out.write("\n=== function %s (%s) ===\n" % (fn.getName(), fn.getEntryPoint()))
                body = fn.getBody()
                it = listing.getInstructions(body, True)
                instrs = []
                while it.hasNext():
                    i2 = it.next()
                    instrs.append((i2.getAddress(), str(i2)))
                # find the ref addr index
                idx = 0
                for k,(a,_) in enumerate(instrs):
                    if a == fa:
                        idx = k; break
                lo = max(0, idx-40); hi = min(len(instrs), idx+8)
                for a, txt in instrs[lo:hi]:
                    mark = " <<<" if a == fa else ""
                    out.write("  %s: %s%s\n" % (a, txt, mark))
out.close()
print("wrote", OUT)
