typedef unsigned int uint32_t; typedef unsigned short uint16_t; typedef int int32_t; typedef unsigned long long uint64_t;
extern int printf(const char *, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern const char *dlerror(void);
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), const void *);
extern int setvbuf(void *, char *, int, uint32_t);
extern void *stdout;
typedef struct hw_module_methods_t { int (*open)(const void *, const char *, void **); } hw_module_methods_t;
typedef struct hw_module_t { uint32_t tag; uint16_t mav, hav; const char *id, *name, *author; const hw_module_methods_t *methods; void *dso; uint32_t res[25]; } hw_module_t;
typedef struct hw_device_t { uint32_t tag, version; const hw_module_t *module; uint32_t res[12]; int (*close)(void *); } hw_device_t;
typedef struct { int version, numFds, numInts; int data[0]; } native_handle_t;
typedef uint64_t desc_t;
typedef struct g1dev { hw_device_t common; void (*getCaps)(void*,uint32_t*,int32_t*); void *(*getFn)(void*, int32_t); void *dump; void *res[7]; } g1dev;
static void *z[1] = {0};
static const struct { void *a,*b,*c,*d; } st_ __attribute__((used)) = {z,z,z,z};
int gmain(int argc, char **argv, char **envp) {
    setvbuf(stdout, 0, 2, 0);
    void *h = dlopen("/vendor/lib/hw/gralloc.msmnile.so", 2);
    if (!h) { printf("[g] dlopen fail %s\n", dlerror()); return 1; }
    hw_module_t *mod = dlsym(h, "HMI");
    printf("[g] mod id=%s name=%s\n", mod->id, mod->name);
    g1dev *dev = 0;
    int rc = mod->methods->open(mod, "gralloc", (void**)&dev);
    printf("[g] open rc=%d dev=%p\n", rc, dev);
    if (!dev) return 1;
    typedef int32_t (*cfn)(void*, desc_t*);
    typedef int32_t (*dfn)(void*, desc_t, uint32_t, uint32_t);
    typedef int32_t (*ffn)(void*, desc_t, int32_t);
    typedef int32_t (*ufn)(void*, desc_t, uint64_t);
    typedef int32_t (*afn)(void*, uint32_t, const desc_t*, native_handle_t**);
    cfn cd = (cfn)dev->getFn(dev, 2);
    dfn sd = (dfn)dev->getFn(dev, 12);
    ffn sf = (ffn)dev->getFn(dev, 13);
    ufn spu = (ufn)dev->getFn(dev, 14);
    ufn scu = (ufn)dev->getFn(dev, 11);
    afn al = (afn)dev->getFn(dev, 15);
    desc_t d = 0;
    rc = cd(dev, &d);
    printf("[g] create_desc rc=%d d=0x%x\n", rc, d);
    rc = sd(dev, d, 4032, 3024);
    printf("[g] set_dims rc=%d\n", rc);
    rc = sf(dev, d, 0x23);
    printf("[g] set_fmt rc=%d\n", rc);
    rc = spu(dev, d, 0x10000ULL);
    printf("[g] set_pusage rc=%d\n", rc);
    rc = scu(dev, d, 3ULL);
    printf("[g] set_cusage rc=%d\n", rc);
    native_handle_t *nh = 0;
    rc = al(dev, 1, &d, &nh);
    printf("[g] allocate rc=%d nh=%p\n", rc, nh);
    if (nh) printf("[g] nh: fds=%d ints=%d fd0=%d\n", nh->numFds, nh->numInts, nh->data[0]);
    return 0;
}
__attribute__((naked)) void _start(void) {
    __asm__ volatile("mov r0, sp\n mov r1, #0\n .Lb:\n adr r2, .Lb\n ldr r3, .Lo1\n add r2,r2,r3\n adr r3, .Lb\n ldr r12, .Lo2\n add r3,r3,r12\n bl __libc_init\n bkpt #0\n .Lo1: .word gmain - .Lb\n .Lo2: .word st_ - .Lb\n");
}
