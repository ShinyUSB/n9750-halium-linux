typedef unsigned int size_t;
extern int printf(const char *, ...);
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), const void *);
extern int setvbuf(void *, char *, int, size_t);
extern void *stdout;
extern int __system_property_get(const char *, char *);
extern const void *__system_property_find(const char *);
static void *z[1] = {0};
static const struct { void *a, *b, *c, *d; } st_ __attribute__((used)) = { z, z, z, z };
int main_(int argc, char **argv, char **envp) {
    setvbuf(stdout, 0, 2, 0);
    static const char *dflt[] = {
        "ro.hardware",
        "persist.vendor.camera.sync_en",
        "persist.vendor.camera.multiframe_en",
        "persist.vendor.camera.mfnr_en",
        "persist.vendor.camera.fastaec.enable",
        0
    };
    const char **keys = dflt;
    if (argc > 1) keys = (const char **)(argv + 1);
    for (int i = 0; keys[i]; ++i) {
        char buf[128] = {0};
        const void *pi = __system_property_find(keys[i]);
        int rc = __system_property_get(keys[i], buf);
        printf("[pt] %s: found=%s rc=%d val='%s'\n", keys[i], pi ? "yes" : "no", rc, buf);
    }
    return 0;
}
__attribute__((naked)) void _start(void) {
    __asm__ volatile(
        "mov r0, sp\n mov r1, #0\n .Lb:\n adr r2, .Lb\n ldr r3, .Lo1\n add r2, r2, r3\n"
        " adr r3, .Lb\n ldr r12, .Lo2\n add r3, r3, r12\n bl __libc_init\n bkpt #0\n"
        ".Lo1: .word main_ - .Lb\n .Lo2: .word st_ - .Lb\n");
}
