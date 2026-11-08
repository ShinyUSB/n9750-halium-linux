# -*- coding: utf-8 -*-
# Full decompile of SecCamTOFUsecase::Create (0x1cb944) + the pointcloud branch
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_tof_create_full.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)
asm = program.getAddressFactory().getDefaultAddressSpace()

fn = fm.getFunctionAt(asm.getAddress(0x1cb944))
res = di.decompileFunction(fn, 600, monitor)
c = res.getDecompiledFunction().getC()
out.write(c)
out.write("\n\n===== END (len %d) =====\n" % len(c))
out.close()
print("wrote", OUT, len(c))
