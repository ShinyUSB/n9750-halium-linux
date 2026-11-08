# -*- coding: utf-8 -*-
# Decompile TOFProcPlugin::set_raw_info + config check in libTOFProc_interface.so
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/tofproc_raw.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

def decomp(fn, cap=12000):
    res = di.decompileFunction(fn, 180, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

# 1. by name
done = set()
for fn in fm.getFunctions(True):
    nm = fn.getName()
    if "set_raw_info" in nm or "SetConfigInfo" in nm or "set_config" in nm.lower():
        done.add(fn.getEntryPoint().getOffset())
        out.write("\n===== %s (%s) =====\n" % (nm, fn.getEntryPoint()))
        out.write(decomp(fn))

# 2. by the two strings
for want in ["Size of inputRaw is not supported", "input Raw Size", "raw_info.size incorrect"]:
    for d in listing.getDefinedData(True):
        try:
            sval = str(d.getValue())
        except Exception:
            continue
        if sval and want in sval:
            for ref in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
                f = fm.getFunctionContaining(ref.getFromAddress())
                if f and f.getEntryPoint().getOffset() not in done:
                    done.add(f.getEntryPoint().getOffset())
                    out.write("\n===== %s (%s) <<%s>> =====\n" % (f.getName(), f.getEntryPoint(), want))
                    out.write(decomp(f))

out.close()
print("wrote", OUT, len(done))
