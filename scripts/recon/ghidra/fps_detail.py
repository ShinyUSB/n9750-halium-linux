# -*- coding: utf-8 -*-
# Find which global getSensorDimension defaults to 30 (0x1e)
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_fpsdetail.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()

f = fm.getFunctionAt(toAddr(0x8a8a4))
if f is None:
    f = fm.getFunctionContaining(toAddr(0x8a8a4))
out.write("function: %s body=%s\n" % (f, f.getBody()))

ins_iter = listing.getInstructions(f.getBody(), True)
lines = []
for ins in listing.getInstructions(f.getBody(), True):
    lines.append((ins.getAddress().getOffset(), ins.toString(), [r.getToAddress().getOffset() for r in ins.getReferencesFrom()]))

for i, (off, txt, refs) in enumerate(lines):
    out.write("0x%x: %-44s refs=%s\n" % (off, txt, refs))
out.close()
print("DONE -> " + OUT)
