# -*- coding: utf-8 -*-
# Resolve property_get arguments in the usecase-matching region
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_props2.txt"
out = open(OUT, "w")
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refmgr = currentProgram.getReferenceManager()

# find property_get function
pg = None
for f in fm.getFunctions(True):
    if "property_get" in f.getName():
        pg = f
        break
out.write("property_get func: %s @ %s\n" % (pg, pg.getEntryPoint() if pg else None))
if pg:
    # for each call site, resolve r0 arg: find the ldr before that loads a string address
    for ref in refmgr.getReferencesTo(pg.getEntryPoint()):
        calladdr = ref.getFromAddress()
        f2 = fm.getFunctionContaining(calladdr)
        # scan backward up to 10 instructions for a reference to a defined string
        addr = calladdr
        found = []
        for k in range(10):
            p = listing.getInstructionBefore(addr)
            if p is None:
                break
            for rr in p.getReferencesFrom():
                t = rr.getToAddress()
                d = listing.getDataAt(t)
                if d is not None:
                    try:
                        v = d.getValue()
                    except Exception:
                        v = None
                    if (isinstance(v, str) or isinstance(v, unicode)) and len(str(v)) > 3:
                        found.append((p.getAddress().getOffset(), str(v)[:80]))
            addr = p.getAddress()
        if found:
            out.write("call @ 0x%x in %s:\n" % (calladdr.getOffset(), f2.getName() if f2 else "?"))
            for fa, fs in found[:3]:
                out.write("   0x%x: %s\n" % (fa, fs))
out.close()
print("DONE -> " + OUT)
