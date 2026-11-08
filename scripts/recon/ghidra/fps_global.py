# -*- coding: utf-8 -*-
# Find the FPS global used by getSensorDimension, then find all code writing it
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/chi_fpsglobals.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
listing = program.getListing()
di = DecompInterface()
di.openProgram(program)

# memory blocks: writable ranges
mem = program.getMemory()
rw = []
for b in mem.getBlocks():
    if b.isWrite() and b.getSize() < 0x400000:
        rw.append((b.getStart().getOffset(), b.getEnd().getOffset(), b.getName()))
out.write("RW blocks: %s\n" % rw)

def in_rw(a):
    o = a.getOffset()
    for s, e, n in rw:
        if s <= o < e:
            return True
    return False

f = fm.getFunctionAt(toAddr(0x8a8a4))
if f is None:
    f = fm.getFunctionContaining(toAddr(0x8a8a4))
out.write("function: %s\n" % f)
# gather all reference targets from this function that land in RW memory
globals_seen = {}
inst_iter = listing.getInstructions(f.getBody(), True)
for ins in inst_iter:
    for ref in ins.getReferencesFrom():
        t = ref.getToAddress()
        if in_rw(t):
            globals_seen[t.getOffset()] = ins.toString()

for g, ins in sorted(globals_seen.items()):
    out.write("global 0x%x written/read via: %s\n" % (g, ins[:80]))

# find the FPS one: the decompiler said it's added with 0x8c384 — pick candidates
# instead: enumerate ALL code references to each candidate global in the WHOLE program
for g in sorted(globals_seen):
    writers = []
    refs = getReferencesTo(toAddr(g))
    for r in refs:
        fa = r.getFromAddress()
        fn = fm.getFunctionContaining(fa)
        if fn is not None:
            # check if it's a write (approx: instruction is str/vstr)
            ins = listing.getInstructionAt(fa)
            txt = ins.toString() if ins else ""
            if txt.startswith("str") or "st" in txt[:4]:
                writers.append((fn.getName(), fa.getOffset(), txt[:60]))
    if writers:
        out.write("\n=== global 0x%x — %d code refs, %d writes:\n" % (g, len(list(refs)), len(writers)))
        for wn, off, txt in writers[:10]:
            out.write("  write in %s @ 0x%x: %s\n" % (wn, off, txt))

out.close()
print("DONE -> " + OUT)
