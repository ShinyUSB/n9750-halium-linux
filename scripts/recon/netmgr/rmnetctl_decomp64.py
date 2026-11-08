# -*- coding: utf-8 -*-
# Full decomp of 64-bit librmnetctl exports (wire format recovery).
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/netmgr/rmnetctl64_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

WANT = ["rmnetctl_init", "rmnetctl_transact", "rmnet_set_link_egress_data_format",
        "rmnet_set_link_ingress_data_format_tailspace", "rmnet_get_link_egress_data_format",
        "rmnet_new_vnd", "rmnet_new_vnd_prefix", "rmnet_new_vnd_name",
        "rmnet_associate_network_device", "rmnet_set_logical_ep_config",
        "rmnet_get_vnd_name", "rmnetctl_cleanup"]

def decomp(fn, cap=20000):
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
    for w in WANT:
        if nm == w or (w == "rmnetctl_transact" and "transact" in nm):
            if fn.getEntryPoint().getOffset() not in dumped:
                dumped.add(fn.getEntryPoint().getOffset())
                out.write("\n===== %s @ %s =====\n" % (nm, fn.getEntryPoint()))
                out.write(decomp(fn))
            break
out.close()
print("wrote", OUT, len(dumped))
