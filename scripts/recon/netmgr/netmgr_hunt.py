# -*- coding: utf-8 -*-
# Hunt netmgrd rmnet/ipa init flow: find functions by log strings + import xrefs.
# @category: RE
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.util import DefinedDataIterator
from ghidra.program.model.symbol import RefType

OUT = "/home/usb/mnt-sda/camera-recon/netmgr/netmgrd_decomp.txt"
out = open(OUT, "w")
monitor = ConsoleTaskMonitor()
program = currentProgram
fm = program.getFunctionManager()
di = DecompInterface()
di.openProgram(program)

NEEDLES = [
    "netmgr_kif_ifioctl_notify_mux_channel",
    "netmgr_kif_ifioctl_set_egress_data_format",
    "netmgr_kif_ifioctl_set_ingress_data_format",
    "Set egress format %d on %s",
    "Set ingress format %d on %s",
    "RmNet returns egress_flags",
    "Updated egress_flags",
    "dataformat_num_mux_channels",
    "dataformat_agg_dl_pkt",
    "dataformat_agg_ul_pkt",
    "u.ingress_format.agg_size",
    "phys_net_dev",
    "rmnet_ipa0",
    "wda_data_format_enabled",
    "Failed to set egress format",
    "Failed to set ingress format",
]

IMPORTS = [
    "rmnet_set_link_egress_data_format",
    "rmnet_get_link_egress_data_format",
    "rmnet_set_link_ingress_data_format_tailspace",
    "rmnet_get_link_ingress_data_format_tailspace",
    "rmnet_new_vnd",
    "rmnet_new_vnd_prefix",
    "rmnet_new_vnd_name",
    "rmnet_get_vnd_name",
    "rmnet_associate_network_device",
    "rmnet_get_network_device_associated",
    "rmnet_set_logical_ep_config",
    "rmnet_unset_logical_ep_config",
    "rmnet_add_del_vnd_tc_flow",
]

def decomp(fn, cap=16000):
    try:
        res = di.decompileFunction(fn, 180, monitor)
        if res and res.getDecompiledFunction():
            return res.getDecompiledFunction().getC()[:cap]
    except Exception as e:
        return "(decomp exc: %s)" % e
    return "(decomp failed)"

def fn_id(fn):
    return fn.getEntryPoint().getOffset()

def callers_of(fn, depth):
    seen = set()
    result = []
    frontier = [fn]
    for _ in range(depth):
        nxt = []
        for f in frontier:
            for c in f.getCallingFunctions(monitor):
                if fn_id(c) not in seen:
                    seen.add(fn_id(c))
                    result.append(c)
                    nxt.append(c)
        frontier = nxt
    return result

dumped = set()

def dump_fn(fn, why, depth=0):
    if fn is None or fn_id(fn) in dumped:
        return
    dumped.add(fn_id(fn))
    out.write("\n===== %s @ %s  [%s] =====\n" % (fn.getName(), fn.getEntryPoint(), why))
    out.write(decomp(fn))

# Pass 1: string needles -> containing function + callers (2 levels)
for needle in NEEDLES:
    hits = 0
    for data in DefinedDataIterator.definedStrings(program):
        v = data.getValue()
        if v is None:
            continue
        s = str(v)
        if needle in s:
            for ref in data.getReferenceIteratorTo():
                f = fm.getFunctionContaining(ref.getFromAddress())
                if f:
                    dump_fn(f, "str:%s" % needle)
                    for c in callers_of(f, 2):
                        dump_fn(c, "caller-of str:%s" % needle)
                    hits += 1
    print("needle", needle, "hits", hits)

# Pass 2: import thunks -> callers + their callers (2 levels)
ext = program.getExternalManager()
for imp in IMPORTS:
    locs = ext.getExternalLocations(imp)
    n = 0
    while locs.hasNext():
        loc = locs.next()
        addr = loc.getAddress()
        if addr is None:
            continue
        for ref in program.getReferenceManager().getReferencesTo(addr):
            f = fm.getFunctionContaining(ref.getFromAddress())
            if f:
                dump_fn(f, "calls %s" % imp)
                for c in callers_of(f, 2):
                    dump_fn(c, "caller2 of %s-user" % imp)
                n += 1
    print("import", imp, "callsites", n)

out.close()
print("wrote", OUT, len(dumped), "functions")
