# -*- coding: utf-8 -*-
# chiTOF: decompile the SecCamTOFUsecase request path: SubmitRealtimeRequest +
# the usecase's ExecuteCaptureRequest (vtable slot after Create/Destroy).
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_submit.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)
asm = program.getAddressFactory().getDefaultAddressSpace()

def decomp(fn, cap=20000):
    res = di.decompileFunction(fn, 240, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

done = set()
for want in ["SubmitRealtimeRequest", "SubmitChiRequest", "chxseccamtofusecase"]:
    for d in listing.getDefinedData(True):
        try:
            sval = str(d.getValue())
        except Exception:
            continue
        if sval and want in sval:
            out.write("# str @%s: %r\n" % (d.getMinAddress(), sval[:110]))
            for ref in program.getReferenceManager().getReferencesTo(d.getMinAddress()):
                f = fm.getFunctionContaining(ref.getFromAddress())
                if f and f.getEntryPoint().getOffset() not in done:
                    done.add(f.getEntryPoint().getOffset())
                    out.write("\n===== %s (%s) <<%s>> =====\n" % (f.getName(), f.getEntryPoint(), want))
                    out.write(decomp(f))

# The usecase vtable at 0x1ef4a0: dump entries 2..12 (virtual methods past the dtors)
out.write("\n== SecCamTOFUsecase vtable (0x1ef4a0) ==\n")
mem = program.getMemory()
for i in range(0, 14):
    a = asm.getAddress(0x1ef4a0 + i * 4)
    try:
        p = mem.getInt(a) & 0xFFFFFFFF
    except Exception:
        break
    fn = fm.getFunctionAt(asm.getAddress(p))
    out.write("vt[%d] = 0x%x  %s\n" % (i, p, fn.getName() if fn else "?"))
    if fn and 2 <= i <= 8 and fn.getEntryPoint().getOffset() not in done:
        done.add(fn.getEntryPoint().getOffset())
        out.write("\n----- vt[%d] %s (%s) -----\n" % (i, fn.getName(), fn.getEntryPoint()))
        out.write(decomp(fn, 12000))

out.close()
print("wrote", OUT, len(done))
