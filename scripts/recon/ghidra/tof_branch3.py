# -*- coding: utf-8 -*-
# Dump GetMatchingUsecase from 0x888d8 onward + the usecase name table.
# @category: RE
OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_branch3.txt"
out = open(OUT, "w")
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refm = program.getReferenceManager()
fn = fm.getFunctionAt(program.getAddressFactory().getDefaultAddressSpace().getAddress(0x8779c))
if fn is None:
    fn = fm.getFunctionContaining(program.getAddressFactory().getDefaultAddressSpace().getAddress(0x888d8))
body = fn.getBody()
it = listing.getInstructions(body, True)
instrs = []
while it.hasNext():
    ins = it.next()
    a = ins.getAddress().getOffset()
    if a >= 0x888d8:
        instrs.append((ins.getAddress(), str(ins)))
out.write("GetMatchingUsecase from 0x888d8:\n")
for a, t in instrs[:160]:
    out.write("  %s: %s\n" % (a, t))
out.write("\n")
# resolve the string refs in that region
out.write("string refs in region 0x888d8-0x88a00:\n")
for d in listing.getDefinedData(True):
    try:
        sval = str(d.getValue())
    except Exception:
        continue
    for r in refm.getReferencesTo(d.getMinAddress()):
        fa = r.getFromAddress().getOffset()
        if 0x888d8 <= fa <= 0x88b00 and len(sval) < 90:
            out.write("  ref %s from 0x%x : %r\n" % (sval, fa))
out.close()
print("wrote", OUT, len(instrs))
