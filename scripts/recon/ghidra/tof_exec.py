# -*- coding: utf-8 -*-
# chiTOF: decompile SecCamTOFUsecase::ExecuteCaptureRequest + processFeatureResult
# to learn which framework streams get raw/depth data.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_exec.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

def decomp(fn, cap=22000):
    res = di.decompileFunction(fn, 240, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

done = set()
# by name
for fn in fm.getFunctions(True):
    nm = fn.getName()
    if ("ExecuteCaptureRequest" in nm or "processFeatureResult" in nm or "ProcessResult" in nm) and "TOF" in nm.upper() or "SecCamTOF" in nm:
        if fn.getEntryPoint().getOffset() not in done:
            done.add(fn.getEntryPoint().getOffset())
            out.write("\n===== %s (%s) =====\n" % (nm, fn.getEntryPoint()))
            out.write(decomp(fn))

# also the ones with telling strings
for want in ["Depth16 Stream is coming", "PointCloud Stream is setted", "Depth Stream is setted",
             "DepthConfidence Stream is coming", "Raw16", "TOFRaw16"]:
    for d in listing.getDefinedData(True):
        try:
            sval = str(d.getValue())
        except Exception:
            continue
        if sval and want in sval:
            out.write("# str @%s: %r\n" % (d.getMinAddress(), sval[:100]))
            for ref in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
                f = fm.getFunctionContaining(ref.getFromAddress())
                if f and f.getEntryPoint().getOffset() not in done:
                    done.add(f.getEntryPoint().getOffset())
                    out.write("\n===== %s (%s) <<%s>> =====\n" % (f.getName(), f.getEntryPoint(), want))
                    out.write(decomp(f))

out.close()
print("wrote", OUT, len(done))
