# -*- coding: utf-8 -*-
# tofproc: disassemble TOFProcPlugin::set_plugin case 0x28 width-check region.
# @category: RE
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/tofproc_disasm.txt"
out = open(OUT, "w")
program = currentProgram
listing = program.getListing()
asm = program.getAddressFactory().getDefaultAddressSpace()

# disassemble a range
start = asm.getAddress(0x15d00)
end = asm.getAddress(0x15f60)
a = start
while a <= end:
    ins = listing.getInstructionAt(a)
    if ins:
        out.write("%s: %s\n" % (a, ins.toString()))
        a = a.add(ins.getLength())
    else:
        a = a.add(2)
out.close()
print("wrote", OUT)
