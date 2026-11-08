/* propdump: list all properties via the real bionic reader.
 * usage: propdump [prefix-filter] */
typedef unsigned int size_t;
typedef unsigned int uint32_t;
extern int printf(const char *, ...);
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), const void *);
extern int setvbuf(void *, char *, int, size_t);
extern void *stdout;
extern int strncmp(const char *, const char *, size_t);
extern int __system_property_foreach(void (*)(const void *, void *), void *);
extern void __system_property_read_callback(const void *,
    void (*)(void *, const char *, const char *, uint32_t), void *);

static const char *g_filter;
static int g_filter_len;
static int g_count;

static void nameval_cb(void *cookie, const char *name, const char *value, uint32_t serial) {
    (void)cookie; (void)serial;
    if (g_filter) {
        int i;
        for (i = 0; i < g_filter_len; i++)
            if (name[i] != g_filter[i]) return;
    }
    printf("%s=%s\n", name, value);
    g_count++;
}

static void foreach_cb(const void *pi, void *cookie) {
    __system_property_read_callback(pi, nameval_cb, cookie);
}

static void *z[1] = {0};
static const struct { void *a, *b, *c, *d; } st_ __attribute__((used)) = { z, z, z, z };
int main_(int argc, char **argv, char **envp) {
    (void)envp;
    setvbuf(stdout, 0, 2, 0);
    if (argc > 1) {
        g_filter = argv[1];
        while (g_filter[g_filter_len]) g_filter_len++;
    }
    int rc = __system_property_foreach(foreach_cb, 0);
    printf("[pd] foreach rc=%d, printed %d props (filter='%s')\n",
           rc, g_count, g_filter ? g_filter : "");
    return 0;
}
__attribute__((naked)) void _start(void) {
    __asm__ volatile(
        "mov r0, sp\n mov r1, #0\n .Lb:\n adr r2, .Lb\n ldr r3, .Lo1\n add r2, r2, r3\n"
        " adr r3, .Lb\n ldr r12, .Lo2\n add r3, r3, r12\n bl __libc_init\n bkpt #0\n"
        ".Lo1: .word main_ - .Lb\n .Lo2: .word st_ - .Lb\n");
}
