# -*- coding: utf-8 -*-
# Decompile the CCI qmux xport machinery.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "/home/usb/mnt-sda/camera-recon/netmgr/cci_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

WANT = ["qmi_cci_qmux_xport_register", "qmi_cci_qmux_xport_unregister",
        "qmi_cci_xport_start", "qmi_cci_xport_recv", "qmi_client_init_instance",
        "qmi_client_init", "qmi_client_get_any_service", "qmi_client_get_service_list"]

def decomp(fn, cap=24000):
    try:
        res = di.decompileFunction(fn, 180, monitor)
        if res and res.getDecompiledFunction():
            return res.getDecompiledFunction().getC()[:cap]
    except Exception as e:
        return "(exc %s)" % e
    return "(failed)"

dumped = set()
for fn in fm.getFunctions(True):
    nm = fn.getName()
    for w in WANT:
        if nm == w and fn.getEntryPoint().getOffset() not in dumped:
            dumped.add(fn.getEntryPoint().getOffset())
            out.write("\n===== %s @ %s =====\n" % (nm, fn.getEntryPoint()))
            out.write(decomp(fn))
            # also dump direct callees with 'qmux' or 'xport' in name
            for c in fn.getCalledFunctions(monitor):
                cn = c.getName()
                if ("qmux" in cn or "xport" in cn or "ipcr" in cn or "qrtr" in cn) and c.getEntryPoint().getOffset() not in dumped:
                    dumped.add(c.getEntryPoint().getOffset())
                    out.write("\n===== %s @ %s [callee of %s] =====\n" % (cn, c.getEntryPoint(), nm))
                    out.write(decomp(c))
            break
out.close()
print("wrote", OUT, len(dumped))
