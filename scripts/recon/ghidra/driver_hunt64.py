# -*- coding: utf-8 -*-
# Find dlopen calls in ExtensionModule constructor (64-bit CHI) and resolve the library path
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi64_driver.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refmgr = program.getReferenceManager()
di = DecompInterface()
di.openProgram(program)

# find the string "CHI Opened driver library" 
target = "CHI Opened driver library"
funcs = {}
for d in listing.getDefinedData(True):
    try:
        v = d.getValue()
    except Exception:
        continue
    if not (isinstance(v, str) or isinstance(v, unicode)):
        continue
    if target in str(v):
        addr = d.getAddress()
        out.write("string at %s\n" % addr)
        # 2-hop to find referencing functions
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
                    funcs[f] = True
                else:
                    todo.append(fa)

# also find dlopen function
dlopen_f = None
for f in fm.getFunctions(True):
    if "dlopen" in f.getName():
        dlopen_f = f
        break
out.write("dlopen: %s\n" % dlopen_f)

# find functions that call dlopen AND reference our string
if dlopen_f:
    dlopen_callers = set()
    for ref in refmgr.getReferencesTo(dlopen_f.getEntryPoint()):
        fa = ref.getFromAddress()
        f = fm.getFunctionContaining(fa)
        if f:
            dlopen_callers.add(f)
    out.write("dlopen callers: %d\n" % len(dlopen_callers))
    # intersect with string-referencing functions
    both = dlopen_callers & set(funcs.keys())
    out.write("functions with both dlopen + string: %d\n" % len(both))
    for f in both:
        res = di.decompileFunction(f, 120, monitor)
        out.write("\n// %s @ %s\n" % (f.getName(), f.getEntryPoint()))
        if res.decompileCompleted():
            out.write(res.getDecompiledFunction().getC())
        else:
            out.write("// fail\n")

# Also: decompile ALL functions that call dlopen and are SMALL (likely the driver open)
if dlopen_f:
    count = 0
    for f in sorted(dlopen_callers, key=lambda x: x.getBody().getNumAddresses()):
        sz = f.getBody().getNumAddresses()
        if sz < 2000 and count < 15:
            res = di.decompileFunction(f, 60, monitor)
            if res.decompileCompleted():
                c = res.getDecompiledFunction().getC()
                if "dlopen" in c or "CHI Opened" in c or "driver" in c.lower():
                    out.write("\n// small dlopen caller: %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), sz))
                    out.write(c)
                    count += 1
out.close()
print("DONE -> " + OUT)
