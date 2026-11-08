# Ghidra headless: resolve strings -> literal pools -> code, decompile containing functions
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_ux60.txt"
TARGETS = [
    "Enable perflock for UHD60",
    "Enable perflock for UHD30",
    "Enable perflock for FHD60",
    "Enable perflock for FHD30",
    "SelectedMode W=",
    "Default sensor FPS",
    "FPS Range ",
    "Set Custom Usecase, selectedUsecaseId",
    "Matching Usecase id = ",
]

out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

# build string map: address -> value
strmap = {}
for d in listing.getDefinedData(True):
    try:
        v = d.getValue()
    except Exception:
        continue
    if isinstance(v, str) or isinstance(v, unicode):
        strmap[d.getAddress()] = str(v)

def code_refs_to(addr):
    """2-hop: code -> pool -> string, or direct code -> string"""
    seen = set()
    todo = [addr]
    result = []
    while todo:
        a = todo.pop()
        for r in getReferencesTo(a):
            fa = r.getFromAddress()
            if fa in seen:
                continue
            seen.add(fa)
            f = fm.getFunctionContaining(fa)
            if f is not None:
                result.append((f, fa))
            else:
                # pool word inside .text but outside function: chase one more hop
                todo.append(fa)
    return result

funcs = {}
for d in listing.getDefinedData(True):
    try:
        v = d.getValue()
    except Exception:
        continue
    if not (isinstance(v, str) or isinstance(v, unicode)):
        continue
    sval = str(v)
    for t in TARGETS:
        if t in sval:
            addr = d.getAddress()
            for f, fa in code_refs_to(addr):
                funcs.setdefault(f, set()).add(sval[:60])
            break

print("functions found: %d" % len(funcs))
count = 0
for f, strs in sorted(funcs.items(), key=lambda kv: kv[0].getBody().getNumAddresses()):
    if count > 40:
        break
    count += 1
    try:
        res = di.decompileFunction(f, 90, monitor)
        out.write("\n//================ %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
        out.write("// strings: %s\n" % " | ".join(sorted(strs)))
        if res.decompileCompleted():
            out.write(res.getDecompiledFunction().getC())
        else:
            out.write("// decompile failed: %s\n" % res.getErrorMessage())
    except Exception as e:
        out.write("// exception: %s\n" % str(e))
out.close()
print("DONE -> " + OUT)
