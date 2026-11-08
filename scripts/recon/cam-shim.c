// cam-shim: standalone bionic driver for Samsung's camera.qcom.so (CamX).
// Milestone 1: enumerate cameras + get_camera_info (probes sensors via kernel).
//
// Build:  clang --target=aarch64-linux-android -nostdlib -fuse-ld=lld \
//           -o cam-shim cam-shim.c /tmp/bionic-pull/libc.so /tmp/bionic-pull/libdl.so
// Run:    LD_LIBRARY_PATH=/vendor/lib64:/opt/bionic/system/lib64:/opt/bionic/vendor/lib64 \
//           /opt/bionic/system/linker64 /tmp/cam-shim
//
// No libc headers: minimal ABI declarations only.

typedef unsigned long size_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef int int32_t;

extern int printf(const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern const char *dlerror(void);
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), const void *);
extern void exit(int);

// ---- hardware.h / camera3.h minimal ABI ----

typedef struct hw_module_methods_t {
    int (*open)(const void *module, const char *id, void **device);
} hw_module_methods_t;

typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    const hw_module_methods_t *methods;
    void *dso;
    uint32_t reserved[49]; /* Samsung pads hw_module_t to 248 bytes (measured
                            * from HMI reloc slots in camera.qcom.so) */
} hw_module_t;

typedef struct camera_info {
    int facing;
    int orientation;
    uint32_t device_version;
    const void *static_camera_characteristics;
    int resource_cost;
    char **conflicting_devices;
    size_t conflicting_devices_length;
} camera_info_t;

typedef struct camera_module {
    hw_module_t common;                                      /* 248 bytes */
    int (*get_number_of_cameras)(void);                      /* +0xf8 */
    int (*get_camera_info)(int camera_id, camera_info_t *);  /* +0x100 */
    int (*set_callbacks)(void *callbacks);                   /* +0x108 */
    void (*get_vendor_tag_ops)(void *ops);                   /* +0x110 */
    int (*open_legacy)(const void *, const char *, int, void **); /* +0x118 */
    /* set_torch_mode/init/get_physical_camera_info absent in this build */
} camera_module_t;

// ---- entry ----

static void *zero_structors[1] = { 0 };
static const struct {
    void *preinit, *init, *fini, *term;
} structors = { zero_structors, zero_structors, zero_structors, zero_structors };

extern int setvbuf(void *stream, char *buf, int mode, size_t size);
extern void *stdout; /* bionic exports stdout as a FILE* object */

static int shim_main(int argc, char **argv, char **envp) {
    /* unbuffer stdout: a crash must not eat the log */
    setvbuf(stdout, (void *)0, 2 /*_IONBF*/, 0);
    const char *path = "/vendor/lib64/hw/camera.qcom.so";
    printf("[shim] dlopen %s\n", path);
    void *h = dlopen(path, 2 /* RTLD_NOW */);
    if (!h) {
        printf("[shim] dlopen FAILED: %s\n", dlerror());
        return 1;
    }
    printf("[shim] dlopen ok\n");

    hw_module_t *mod = (hw_module_t *)dlsym(h, "HMI");
    if (!mod) {
        printf("[shim] no HMI symbol: %s\n", dlerror());
        return 1;
    }
    printf("[shim] HMI: id=%s name=%s api=%u.%u\n",
           mod->id ? mod->id : "?", mod->name ? mod->name : "?",
           mod->module_api_version >> 8, mod->module_api_version & 0xff);

    camera_module_t *cam = (camera_module_t *)mod;
    int n = cam->get_number_of_cameras();
    printf("[shim] number_of_cameras = %d\n", n);
    for (int i = 0; i < n && i < 12; i++) {
        camera_info_t info;
        int rc = cam->get_camera_info(i, &info);
        if (rc != 0) {
            printf("[shim] camera %d: get_camera_info rc=%d\n", i, rc);
            continue;
        }
        const char *facing = info.facing == 0 ? "BACK" : info.facing == 1 ? "FRONT" : "EXT";
        printf("[shim] camera %d: facing=%s orient=%d devver=%u.%u staticmeta=%p\n",
               i, facing, info.orientation,
               info.device_version >> 8, info.device_version & 0xff,
               info.static_camera_characteristics);
    }
    printf("[shim] DONE\n");
    return 0;
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile(
        "mov x0, sp\n"
        "mov x1, xzr\n"
        "adrp x2, %c0\n"
        "add x2, x2, :lo12:%c0\n"
        "adrp x3, %c1\n"
        "add x3, x3, :lo12:%c1\n"
        "bl __libc_init\n"
        "brk #0\n"
        :: "i"(shim_main), "i"(&structors));
}
