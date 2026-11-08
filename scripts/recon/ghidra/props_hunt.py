# -*- coding: utf-8 -*-
# Resolve strings referenced by property_get in GetMatchingUsecase
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_props.txt"
out = open(OUT, "w")
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
f = fm.getFunctionAt(toAddr(0x9779c))   # 0x8779c + 0x10000 rebase
if f is None:
    f = fm.getFunctionContaining(toAddr(0x9779c))
out.write("func: %s\n" % f)

# walk instructions; find calls to property_get; resolve both string args via references
for ins in listing.getInstructions(f.getBody(), True):
    if "property_get" in ins.toString() or "BL" == ins.getMnemonicString() and False:
        pass
    mn = ins.getMnemonicString()
    if mn == "bl":
        for ref in ins.getReferencesFrom():
            tgt = ref.getToAddress()
            tf = fm.getFunctionAt(tgt)
            if tf and "property_get" in tf.getName():
                out.write("call at 0x%x\n" % ins.getAddress().getOffset())
                # r0/r1 were loaded by earlier ldr [pc] — scan refs of nearby instructions
                for back in range(1, 12):
                    prev = listing.getInstructionBefore(ins.getAddress())
                # simpler: dump refs from the 6 instructions before
                addr = ins.getAddress()
                for k in range(6):
                    p = listing.getInstructionBefore(addr)
                    if p is None: break
                    for rr in p.getReferencesFrom():
                        t2 = rr.getToAddress()
                        d = listing.getDataAt(t2)
                        if d is not None:
                            try:
                                v = d.getValue()
                            except Exception:
                                v = None
                            if isinstance(v, str) or isinstance(v, unicode):
                                out.write("  arg str @0x%x: %s\n" % (p.getAddress().getOffset(), str(v)[:60]))
                    addr = p.getAddress()
out.close()
print("DONE -> " + OUT)
