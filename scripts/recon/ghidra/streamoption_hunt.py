# -*- coding: utf-8 -*-
# Find streamOption translation in device impl: functions referencing "streamOption" strings,
# plus any function writing camera3_stream fields from a HIDL stream.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/devimpl_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

funcs = {}
for d in listing.getDefinedData(True):
    try:
        v = d.getValue()
    except Exception:
        continue
    if not (isinstance(v, str) or isinstance(v, unicode)):
        continue
    sval = str(v)
    if any(k in sval for k in ("streamOption", "stream_option", "physicalCameraId",
                                 "Invalid physical camera", "configureStreams",
                                 "StreamConfiguration", "fmq")):
        addr = d.getAddress()
        # hop: string -> refs -> containing functions (2 hops for pointers)
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
                    funcs.setdefault(f, set()).add(sval[:60])
                else:
                    todo.append(fa)

print("functions: %d" % len(funcs))
count = 0
for f, strs in sorted(funcs.items(), key=lambda kv: -kv[0].getBody().getNumAddresses()):
    if count > 30:
        break
    count += 1
    try:
        res = di.decompileFunction(f, 120, monitor)
        out.write("\n//================ %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
        out.write("// strings: %s\n" % " | ".join(sorted(strs)))
        if res.decompileCompleted():
            out.write(res.getDecompiledFunction().getC())
        else:
            out.write("// fail: %s\n" % res.getErrorMessage())
    except Exception as e:
        out.write("// exception %s\n" % str(e))
out.close()
print("DONE -> " + OUT)
