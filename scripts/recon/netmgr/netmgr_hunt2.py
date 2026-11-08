# -*- coding: utf-8 -*-
# Targeted full decomp of netmgrd rmnet/ipa init functions + callers.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/netmgr/netmgrd_decomp2.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

TARGETS = [
    "netmgr_kif_extended_ioctl",
    "netmgr_kif_ifioctl_get_features",
    "netmgr_kif_ifioctl_set_ingress_data_format",
    "netmgr_kif_init_iface",
    "netmgr_rmnet_convert_rmnet_edf_to_ioctl_edf",
    "netmgr_rmnet_convert_rmnet_idf_to_ioctl_idf",
    "netmgr_rmnet_update_ul_aggregation",
    "netmgr_kif_init",
    "netmgr_rmnet_init",
]
CALLERS_OF = [
    "netmgr_kif_set_data_format",
    "netmgr_rmnet_set_feature_flags",
    "netmgr_rmnet_new_vnd",
    "netmgr_kif_ifioctl_notify_mux_channel",
]

def decomp(fn, cap=60000):
    try:
        res = di.decompileFunction(fn, 240, monitor)
        if res and res.getDecompiledFunction():
            return res.getDecompiledFunction().getC()[:cap]
    except Exception as e:
        return "(decomp exc: %s)" % e
    return "(decomp failed)"

dumped = set()
def dump(fn, why):
    if fn is None or fn.getEntryPoint().getOffset() in dumped:
        return
    dumped.add(fn.getEntryPoint().getOffset())
    out.write("\n===== %s @ %s [%s] =====\n" % (fn.getName(), fn.getEntryPoint(), why))
    out.write(decomp(fn))

byname = {}
for fn in fm.getFunctions(True):
    byname.setdefault(fn.getName(), fn)

for t in TARGETS:
    fn = byname.get(t)
    if fn:
        dump(fn, "target")
    else:
        out.write("\n===== %s NOT FOUND =====\n" % t)
        print("not found:", t)

for t in CALLERS_OF:
    fn = byname.get(t)
    if not fn:
        out.write("\n===== %s NOT FOUND (caller query) =====\n" % t)
        continue
    for c in fn.getCallingFunctions(monitor):
        dump(c, "caller of %s" % t)

out.close()
print("wrote", OUT, len(dumped))
