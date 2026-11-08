# Decompile netmgrd DPM functions to recover the endpoint id mapping for DPM open port.
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# nm mini.elf vaddrs; Ghidra rebases PIE to 0x100000
TARGETS = [
    0x9f148,   # netmgr_qmi_dpm_initalize
    0xa1e40,   # netmgr_qmi_dpm_initialize_blocking
]

fm = currentProgram.getFunctionManager()
decomp = DecompInterface()
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

for va in TARGETS:
    addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(va + 0x100000)
    fn = fm.getFunctionContaining(addr)
    if fn is None:
        print("=== NO FUNCTION at 0x%x ===" % va)
        continue
    res = decomp.decompileFunction(fn, 180, monitor)
    print("=== 0x%x %s ===" % (va, fn.getName()))
    if res and res.decompiledFunction:
        print(res.decompiledFunction.getC())
    else:
        print("DECOMPILE FAILED")
    print("=== END 0x%x ===" % va)
