# -*- coding: utf-8 -*-
# Pass 4: change_embedded_links + create_muxed_channels + callers.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/netmgr/netmgrd_decomp4.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

TARGETS = ["netmgr_rmnet_change_embedded_links", "netmgr_rmnet_create_muxed_channels",
           "netmgr_kif_vnd_cleanup_then_notify_clients", "netmgr_rmnet_query_vnd_name"]
CALLERS_OF = ["netmgr_rmnet_create_muxed_channels", "netmgr_qmi_wds_bind_mux_data_port"]

def decomp(fn, cap=80000):
    try:
        res = di.decompileFunction(fn, 240, monitor)
        if res and res.getDecompiledFunction():
            return res.getDecompiledFunction().getC()[:cap]
    except Exception as e:
        return "(exc %s)" % e
    return "(failed)"

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
    if fn: dump(fn, "target")
    else: out.write("\n== %s NOT FOUND ==\n" % t)
for t in CALLERS_OF:
    fn = byname.get(t)
    if fn:
        for c in fn.getCallingFunctions(monitor):
            dump(c, "caller of %s" % t)
out.close()
print("wrote", OUT, len(dumped))
