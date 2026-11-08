// ioctl-spy: LD_PRELOAD interposer dumping VIDIOC_CAM_CONTROL traffic.
// Build: clang --target=aarch64-linux-android -nostdlib -fuse-ld=lld -shared \
//        -fPIC -o ioctl-spy.so ioctl-spy.c /tmp/bionic-pull/libc.so /tmp/bionic-pull/libdl.so
#include <stdarg.h>

typedef unsigned long size_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

extern int snprintf(char *, size_t, const char *, ...);
extern long write(int, const void *, unsigned long);
extern void *dlvsym(void *, const char *, const char *);
extern void *dlsym(void *, const char *);
extern int sprintf(char *, const char *, ...);

#define CAM_CTRL_REQ 0xC01856C0uL /* _IOWR('V', 0xc0, 0x18) */

static long (*real_ioctl)(int, unsigned long, void *) = 0;
static int spy_fd = 2;

static void say(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    write(spy_fd, s, n);
}

static void hexline(int fd, const char *tag, uint32_t op, uint32_t size,
                    uint32_t htype, const unsigned char *p, int rc) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "[spy] fd=%d %s op=0x%x size=%u htype=%u rc=%d data=",
                     fd, tag, op, size, htype, rc);
    int room = sizeof(buf) - n - 2;
    int show = size < 48 ? size : 48;
    if (show * 3 > room) show = room / 3;
    for (int i = 0; i < show; i++) {
        buf[n++] = "0123456789abcdef"[p[i] >> 4];
        buf[n++] = "0123456789abcdef"[p[i] & 15];
        buf[n++] = ' ';
    }
    buf[n++] = '\n';
    write(spy_fd, buf, n);
}

/* variadic ioctl interpose */
long ioctl(int fd, unsigned long req, ...) {
    void *arg;
    __builtin_va_list ap;
    __builtin_va_start(ap, req);
    arg = __builtin_va_arg(ap, void *);
    __builtin_va_end(ap);

    if (!real_ioctl) {
        extern void *dlopen(const char *, int);
        void *h = dlopen("libc.so", 1);
        /* bionic ioctl is varargs libc symbol */
        real_ioctl = (long (*)(int, unsigned long, void *))dlsym(h, "ioctl");
        if (!real_ioctl) {
            say("[spy] FATAL: no real ioctl\n");
            return -1;
        }
    }

    int interesting = (req == CAM_CTRL_REQ);
    uint32_t op = 0, size = 0, htype = 0;
    const unsigned char *payload = 0;
    if (interesting && arg) {
        uint32_t *c = (uint32_t *)arg;
        op = c[0]; size = c[1]; htype = c[2];
        uint64_t handle = *(uint64_t *)((char *)arg + 8);
        if (htype == 0 && handle) payload = (const unsigned char *)(unsigned long)handle;
        if (payload) hexline(fd, "SEND", op, size, htype, payload, 0);
        else hexline(fd, "SEND", op, size, htype, (const unsigned char *)"", 0);
    }
    long rc = real_ioctl(fd, req, arg);
    if (interesting && payload) {
        hexline(fd, "RECV", op, size, htype, payload, (int)rc);
    }
    return rc;
}
