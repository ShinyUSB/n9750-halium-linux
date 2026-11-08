# Ghidra headless postScript: decompile the ipacm DL-filter functions.
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

TARGETS = [
    0x54188,  # IPACM_Wan::install_wan_filtering_rule(bool)
    0x704ec,  # IPACM_Wan::add_dft_filtering_rule
    0x287e4,  # IPACM_Filtering::AddWanDLFilteringRule
    0x57a48,  # IPACM_Wan::handle_addr_evt_mhi_q6
    0x2a3b4,  # IPACM_Filtering::SendFilteringRuleIndex
]

fm = currentProgram.getFunctionManager()
decomp = DecompInterface()
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

for va in TARGETS:
    # Ghidra rebases this PIE binary to 0x100000; nm vaddrs need +0x100000
    addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(va + 0x100000)
    fn = fm.getFunctionContaining(addr)
    if fn is None:
        print("=== NO FUNCTION at 0x%x ===" % va)
        continue
    res = decomp.decompileFunction(fn, 120, monitor)
    print("=== 0x%x %s ===" % (va, fn.getName()))
    if res and res.decompiledFunction:
        print(res.decompiledFunction.getC())
    else:
        print("DECOMPILE FAILED")
    print("=== END 0x%x ===" % va)
