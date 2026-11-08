# Decompile the netmgrd function containing 0xa0ef0 (dpm_port_open) to recover
# the exact hw_data_rx_id / hw_data_tx_id values stock sends in DPM open port.
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

fm = currentProgram.getFunctionManager()
decomp = DecompInterface()
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

for va in [0xa0ef0]:
    addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(va + 0x100000)
    fn = fm.getFunctionContaining(addr)
    print("=== function containing 0x%x: %s @ %s ===" % (va, fn.getName(), fn.getEntryPoint()))
    res = decomp.decompileFunction(fn, 240, monitor)
    if res and res.decompiledFunction:
        print(res.decompiledFunction.getC())
    else:
        print("DECOMPILE FAILED")
