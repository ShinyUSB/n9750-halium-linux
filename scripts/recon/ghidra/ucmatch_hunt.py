# -*- coding: utf-8 -*-
# Resolve strings referenced in GetMatchingUsecase + decompile SecCamVideo usecase matching
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.symbol import RefType

OUT = "/home/usb/mnt-sda/camera-recon/chi_ucmatch.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
refmgr = program.getReferenceManager()
di = DecompInterface()
di.openProgram(program)

def resolve_string_at(addr):
    d = listing.getDataAt(addr)
    if d is not None:
        try:
            v = d.getValue()
            if isinstance(v, str) or isinstance(v, unicode):
                return str(v)
        except Exception:
            pass
        # maybe pointer to string
        try:
            refs = refmgr.getReferencesFrom(addr)
            for r in refs:
                if r.getReferenceType().isData():
                    d2 = listing.getDataAt(r.getToAddress())
                    if d2 is not None:
                        v2 = d2.getValue()
                        if isinstance(v2, str) or isinstance(v2, unicode):
                            return str(v2)
        except Exception:
            pass
    return None

def dump_strings_in(func, tag):
    out.write("\n==== strings referenced by %s ====\n" % tag)
    body = func.getBody()
    count = 0
    addr = body.getMinAddress()
    while addr is not None and count < 500:
        refs = refmgr.getReferencesFrom(addr)
        for r in refs:
            if r.getReferenceType().isData():
                s = resolve_string_at(r.getToAddress())
                if s is not None:
                    out.write("%s -> %r\n" % (addr, s[:250]))
                    count += 1
        nxt = listing.getInstructionAfter(addr)
        if nxt is None or not body.contains(nxt.getAddress()):
            break
        addr = nxt.getAddress()

targets = {}
for f in fm.getFunctions(True):
    name = f.getName()
    if "GetMatchingUsecase" in name and "Thunk" not in name:
        targets["GetMatchingUsecase"] = f
    if "IsMatchingUsecase" in name and "Thunk" not in name:
        targets.setdefault("IsMatchingUsecase", f)
    if "SecCamVideoUsecase" in name and "Thunk" not in name:
        targets.setdefault("SecCamVideo_" + name, f)
    if "SelectUsecaseConfig" in name and "Thunk" not in name:
        targets.setdefault("SelectUsecaseConfig", f)

for tag, f in sorted(targets.items()):
    out.write("\n//================ %s @ %s size=%d\n" % (tag, f.getEntryPoint(), f.getBody().getNumAddresses()))
    dump_strings_in(f, tag)
    res = di.decompileFunction(f, 180, monitor)
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
    else:
        out.write("// decompile failed\n")

out.close()
print("DONE -> " + OUT)
