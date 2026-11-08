# -*- coding: utf-8 -*-
# Decompile librmnetctl.so's rmnet interface setup functions.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/rmnetctl_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

WANT = ["rmnet_set_link_egress_data_format", "rmnet_set_link_ingress_data_format_tailspace",
        "rmnet_new_vnd", "rmnet_new_vnd_prefix", "rmnet_associate_network_device",
        "rmnet_set_logical_ep_config", "rmnet_add_del_vnd_tc_flow"]

def decomp(fn, cap=12000):
    res = di.decompileFunction(fn, 180, monitor)
    if res and res.getDecompiledFunction():
        return res.getDecompiledFunction().getC()[:cap]
    return "(decomp failed)"

done = set()
for fn in fm.getFunctions(True):
    nm = fn.getName()
    for w in WANT:
        if w in nm and fn.getEntryPoint().getOffset() not in done:
            done.add(fn.getEntryPoint().getOffset())
            out.write("\n===== %s (%s) =====\n" % (nm, fn.getEntryPoint()))
            out.write(decomp(fn))
            break
out.close()
print("wrote", OUT, len(done))
