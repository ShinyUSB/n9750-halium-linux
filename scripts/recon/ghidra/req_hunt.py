# -*- coding: utf-8 -*-
# Decompile functions referencing the [REQUEST] stream_type string
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_streamreq.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

TARGET = "[REQUEST] stream_type"
funcs = {}
for d in listing.getDefinedData(True):
    try:
        v = d.getValue()
    except Exception:
        continue
    if not (isinstance(v, str) or isinstance(v, unicode)):
        continue
    if TARGET in str(v):
        addr = d.getAddress()
        todo = [addr]
        seen = set()
        while todo:
            a = todo.pop()
            for r in getReferencesTo(a):
                fa = r.getFromAddress()
                if fa in seen:
                    continue
                seen.add(fa)
                f = fm.getFunctionContaining(fa)
                if f is not None:
                    funcs.setdefault(f, set()).add(str(v)[:60])
                else:
                    todo.append(fa)

out.write("functions: %d\n" % len(funcs))
for f, strs in sorted(funcs.items(), key=lambda kv: -kv[0].getBody().getNumAddresses()):
    res = di.decompileFunction(f, 180, monitor)
    out.write("\n//================ %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
    out.write("// strings: %s\n" % " | ".join(sorted(strs)))
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
    else:
        out.write("// fail: %s\n" % res.getErrorMessage())
out.close()
print("DONE -> " + OUT)
