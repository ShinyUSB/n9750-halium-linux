# Ghidra headless post-script: decompile all functions that reference
# usecase/fps/pipeline-related strings in the CHI override lib.
# @category: RE
import re
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_decomp.txt"
KEY = re.compile(r"usecase|Usecase|fps|FPS|Fps|hfr|HFR|pipeline|Pipeline|streamOption|StreamOption|NonSSM|Multicam|MultiCam|SecMulti|Matching", re.IGNORECASE)

out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

# 1. collect string data matching keywords, map -> referencing functions
funcs = {}
strcount = 0
for d in listing.getDefinedData(True):
    try:
        val = d.getValue()
    except Exception:
        continue
    if not isinstance(val, basestring) and not isinstance(val, str):
        # Ghidra string data getValue returns String for char arrays
        continue
    sval = str(val)
    if len(sval) < 4 or len(sval) > 300:
        continue
    if not KEY.search(sval):
        continue
    strcount += 1
    addr = d.getAddress()
    refs = getReferencesTo(addr)
    for r in refs:
        fa = r.getFromAddress()
        f = fm.getFunctionContaining(fa)
        if f is not None:
            funcs.setdefault(f, set()).add(sval)

print("matched strings: %d, referencing functions: %d" % (strcount, len(funcs)))

# 2. decompile each function once, with the strings it references noted
count = 0
for f, strs in sorted(funcs.items(), key=lambda kv: kv[0].getBody().getNumAddresses()):
    if count > 220:
        out.write("// cap reached\n")
        break
    count += 1
    try:
        res = di.decompileFunction(f, 60, monitor)
        out.write("\n//================ %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
        out.write("// strings: %s\n" % " | ".join(sorted(strs)[:8]))
        if res.decompileCompleted():
            out.write(res.getDecompiledFunction().getC())
        else:
            out.write("// decompile failed: %s\n" % res.getErrorMessage())
    except Exception as e:
        out.write("// exception: %s\n" % str(e))

out.close()
print("DONE: %d functions -> %s" % (count, OUT))
