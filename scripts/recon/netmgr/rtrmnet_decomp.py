# -*- coding: utf-8 -*-
# Decompile rtrmnet_* (RTM channel) functions from librmnetctl64.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/netmgr/rtrmnet64_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

WANT = ["rtrmnet_ctl_init", "rtrmnet_ctl_newvnd", "rtrmnet_ctl_changevnd",
        "rtrmnet_ctl_getvnd", "rtrmnet_set_uplink_aggregation_params"]

def decomp(fn, cap=24000):
    try:
        res = di.decompileFunction(fn, 120, monitor)
        if res and res.getDecompiledFunction():
            return res.getDecompiledFunction().getC()[:cap]
    except Exception as e:
        return "(exc %s)" % e
    return "(failed)"

dumped = set()
for fn in fm.getFunctions(True):
    nm = fn.getName()
    if nm in WANT and fn.getEntryPoint().getOffset() not in dumped:
        dumped.add(fn.getEntryPoint().getOffset())
        out.write("\n===== %s @ %s =====\n" % (nm, fn.getEntryPoint()))
        out.write(decomp(fn))
out.close()
print("wrote", OUT, len(dumped))
