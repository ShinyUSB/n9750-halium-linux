# -*- coding: utf-8 -*-
# Find the SecCamTOFUsecase vtable (via the destructor 0x1cb930's referrer) and
# decompile every virtual method (Initialize, CreateCameraLog, ConfigureStreams, ...).
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.symbol import RefType

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_vtable.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)
asm = program.getAddressFactory().getDefaultAddressSpace()

def decomp(fn):
    res = di.decompileFunction(fn, 180, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()
    return "(decomp failed)"

# the dtor
dtor_addr = asm.getAddress(0x1cb930)
dtor = fm.getFunctionAt(dtor_addr)
out.write("dtor at %s: %s\n" % (dtor_addr, dtor.getName() if dtor else "?"))

# find the vtable: scan defined data for a pointer to the dtor
vtab = None
for d in listing.getDefinedData(True):
    try:
        val = d.getValue()
    except Exception:
        continue
    # Ghidra returns Address for pointer data
    try:
        if val and hasattr(val, "getOffset") and val.getOffset() == 0x1cb930:
            vtab = d.getMinAddress()
            out.write("vtable entry referencing dtor at %s\n" % vtab)
            break
    except Exception:
        continue

if vtab:
    # walk the vtable entries (u32 pointers), decompile each function
    mem = program.getMemory()
    addr = vtab
    for i in range(60):
        try:
            ptr = mem.getInt(addr)
        except Exception:
            break
        if ptr <= 0x10000 or ptr > 0x280000:
            break
        fn = fm.getFunctionAt(asm.getAddress(ptr))
        if fn:
            nm = fn.getName()
            if "unnamed" in nm or nm.startswith("FUN_") or nm == fn.getEntryPoint().toString():
                out.write("\n--- vtable[%d] -> %s (%s) ---\n" % (i, nm, fn.getEntryPoint()))
                out.write(decomp(fn)[:9000])
            else:
                out.write("\n--- vtable[%d] -> %s (%s) ---\n" % (i, nm, fn.getEntryPoint()))
                out.write(decomp(fn)[:9000])
        addr = addr.add(4)
out.close()
print("wrote", OUT)
