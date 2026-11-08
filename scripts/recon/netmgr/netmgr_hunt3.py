# -*- coding: utf-8 -*-
# Third pass: netmgr_qmi_set_data_format + callers + qmi service init context.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/netmgr/netmgrd_decomp3.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

TARGETS = [
    "netmgr_qmi_set_data_format",
    "netmgr_qmi_wds_bind_mux_data_port",
    "netmgr_qmi_wds_start_network",
    "netmgr_qmi_wds_bind_subscription",
]
CALLERS_OF = [
    "netmgr_qmi_set_data_format",
    "netmgr_kif_ifioctl_set_xlat_dev_info",
]

def decomp(fn, cap=80000):
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

for t in TARGETS + CALLERS_OF:
    fn = byname.get(t)
    if fn and t in TARGETS:
        dump(fn, "target")
    if fn and t in CALLERS_OF:
        for c in fn.getCallingFunctions(monitor):
            dump(c, "caller of %s" % t)
    if not fn:
        out.write("\n== %s NOT FOUND ==\n" % t)
        print("not found:", t)

out.close()
print("wrote", OUT, len(dumped))
