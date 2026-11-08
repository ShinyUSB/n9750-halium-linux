# -*- coding: utf-8 -*-
# camera32.qcom.so: decompile the UniDepth node's config path that computes the
# raw input dims passed to TOFProc (SetUniInitParamInfo) + where it reads them.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/unidepth_cfg.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

def decomp(fn, cap=18000):
    res = di.decompileFunction(fn, 240, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

done = set()
WANT_STR = ["SetUniInitParamInfo", "input Raw Size", "UNI_PLUGIN_INDEX_TOF_CONFIG_INFO",
            "TOF_CONFIG_INFO", "rawWidth", "inputRaw"]
for want in WANT_STR:
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

# also by name
for fn in fm.getFunctions(True):
    nm = fn.getName()
    if ("UniDepth" in nm and ("Init" in nm or "Config" in nm or "Param" in nm)) and fn.getEntryPoint().getOffset() not in done:
        done.add(fn.getEntryPoint().getOffset())
        out.write("\n===== %s (%s) [name] =====\n" % (nm, fn.getEntryPoint()))
        out.write(decomp(fn))

out.close()
print("wrote", OUT, len(done))
