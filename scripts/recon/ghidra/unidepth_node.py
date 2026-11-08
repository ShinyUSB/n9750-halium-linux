# -*- coding: utf-8 -*-
# com.samsung.node.uniplugin_depth.so: find where the TOF raw-input dims are set.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/unidepth_node.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

def decomp(fn, cap=16000):
    res = di.decompileFunction(fn, 240, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

done = set()
for want in ["SetUniInitParamInfo", "TOF_CONFIG_INFO", "inputRaw", "CONFIG_INFO"]:
    for d in listing.getDefinedData(True):
        try:
            sval = str(d.getValue())
        except Exception:
            continue
        if sval and want in sval:
            out.write("# str @%s: %r\n" % (d.getMinAddress(), sval[:120]))
            for ref in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
                f = fm.getFunctionContaining(ref.getFromAddress())
                if f and f.getEntryPoint().getOffset() not in done:
                    done.add(f.getEntryPoint().getOffset())
                    out.write("\n===== %s (%s) <<%s>> =====\n" % (f.getName(), f.getEntryPoint(), want))
                    out.write(decomp(f))
out.close()
print("wrote", OUT, len(done))
