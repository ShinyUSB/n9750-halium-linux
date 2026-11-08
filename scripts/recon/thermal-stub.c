// Stub for vendor.samsung.hardware.thermal@1.0.so: getService/tryGetService
// return NULL immediately (no hwservicemanager on Debian).
typedef unsigned char u8;
void *ret_null(void *name, u8 retry);
__asm__(".globl _ZN6vendor7samsung8hardware7thermal4V1_011ISehThermal10getServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb\n"
        ".set _ZN6vendor7samsung8hardware7thermal4V1_011ISehThermal10getServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb, ret_null\n"
        ".globl _ZN6vendor7samsung8hardware7thermal4V1_011ISehThermal13tryGetServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb\n"
        ".set _ZN6vendor7samsung8hardware7thermal4V1_011ISehThermal13tryGetServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb, ret_null");
void *ret_null(void *name, u8 retry) { return 0; }
