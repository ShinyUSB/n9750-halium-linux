/*
 * cap_qrtr_bionic.c — libc-agnostic (raw-syscall, -nostdlib) LD_PRELOAD shim
 * that hex-dumps all AF_QIPCRTR (QRTR, family 42) socket traffic.
 *
 * Unlike the glibc cap_qrtr.c, this makes NO libc calls (only raw syscalls),
 * so the same .so works on bionic (stock Android) AND glibc (Debian).
 *
 * Build (on the phone, gcc): gcc -O2 -nostdlib -shared -fPIC -fno-stack-protector -o cap_qrtr_bionic.so cap_qrtr_bionic.c
 * Use:  LD_PRELOAD=/path/cap_qrtr_bionic.so QRTR_CAP=/tmp/qrtr.cap <daemon>
 * Log format (text): "S <node>:<port> <len> <hex...>" / "R <node>:<port> ...".
 */
typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned int uint32_t;

/* aarch64 raw syscall via svc #0 */
static inline long __sys4(long n, long a, long b, long c, long d) {
	register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d;
	register long x8 asm("x8") = n;
	asm volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x8) : "memory");
	return x0;
}
static inline long __sys5(long n, long a, long b, long c, long d, long e) {
	register long x0 asm("x0")=a, x1 asm("x1")=b, x2 asm("x2")=c, x3 asm("x3")=d, x4 asm("x4")=e;
	register long x8 asm("x8") = n;
	asm volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x8) : "memory");
	return x0;
}
static inline long __sys6(long n, long a, long b, long c, long d, long e, long f) {
	register long x0 asm("x0")=a, x1 asm("x1")=b, x2 asm("x2")=c, x3 asm("x3")=d, x4 asm("x4")=e, x5 asm("x5")=f;
	register long x8 asm("x8") = n;
	asm volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory");
	return x0;
}

#define __NR_openat 56
#define __NR_close 57
#define __NR_write 64
#define __NR_socket 198
#define __NR_sendto 206
#define __NR_recvfrom 207
#define __NR_sendmsg 211
#define __NR_recvmsg 212
#define __NR_getsockname 204
#define __NR_writev 66

#define AF_QIPCRTR 42
static int capfd = -1;

/* minimal env lookup without libc: scan environ for QRTR_CAP= */
extern char **environ;
static const char *getcap(void) {
	static char path[256];
	static const char def[] = "/tmp/qrtr.cap";
	char **e = environ;
	int i;
	for (; e && *e; e++) {
		const char *s = *e;
		if (s[0]=='Q'&&s[1]=='R'&&s[2]=='T'&&s[3]=='R'&&s[4]=='_'&&s[5]=='C'&&s[6]=='A'&&s[7]=='P'&&s[8]=='=')
			return s+9;
	}
	for (i=0;i<(int)sizeof(def);i++) path[i]=def[i];  /* fallthrough copy */
	{ int i; for (i=0;i<(int)sizeof(def);i++) path[i]=def[i]; }
	return path;
}

static void cap_open(void) {
	if (capfd >= 0) return;
	const char *p = getcap();
	/* O_WRONLY|O_CREAT|O_APPEND = 0x441 */
	capfd = (int)__sys4(__NR_openat, -100 /*AT_FDCWD*/, (long)p, 0x441, 0644);
}

static void logline(char dir, unsigned node, unsigned port, const void *buf, size_t len) {
	char hb[16]; char *out; size_t i;
	static char line[32 + 4096*2];
	char *o = line;
	const unsigned char *b = buf;
	if (capfd < 0) return;
	if (len > 2048) len = 2048;
	*o++ = dir; *o++ = ' ';
	/* node */
	if (node == 0) { *o++='0'; } else { /* small int to hex */ }
	/* simple: print node and port as hex */
	const char *hx = "0123456789abcdef";
	unsigned v = node;
	char tmp[8]; int n=0;
	if (!v) *o++='0';
	while (v) { tmp[n++]=hx[v&15]; v>>=4; }
	while (n) *o++=tmp[--n];
	*o++=':';
	v = port; n=0;
	if (!v) *o++='0';
	while (v) { tmp[n++]=hx[v&15]; v>>=4; }
	while (n) *o++=tmp[--n];
	*o++=' ';
	for (i=0;i<len;i++){ unsigned c=b[i]; *o++=hx[c>>4]; *o++=hx[c&15]; }
	*o++='\n';
	__sys4(__NR_write, capfd, (long)line, o-line, 0);
}

/* is this fd a QRTR socket? check via getsockname family */
static int is_qrtr(int fd) {
	unsigned char sa[16];
	unsigned sl = 16;
	long r = __sys4(__NR_getsockname, fd, (long)sa, (long)&sl, 0);
	if (r < 0) return 0;
	return (sa[0] | (sa[1]<<8)) == AF_QIPCRTR;
}

/* the syscall numbers for the socket calls we wrap; we do them raw after logging */
int socket(int dom, int type, int proto) {
	return (int)__sys4(__NR_socket, dom, type, proto, 0);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned alen) {
	if (addr && alen >= 8) {
		const unsigned char *a = addr;
		unsigned fam = a[0] | (a[1]<<8);
		if (fam == AF_QIPCRTR) {
			unsigned node = a[4]|(a[5]<<8)|(a[6]<<16)|(a[7]<<24);
			unsigned port = a[8]|(a[9]<<8)|(a[10]<<16)|(a[11]<<24);
			cap_open(); logline('S', node, port, buf, len);
		}
	}
	return (ssize_t)__sys6(__NR_sendto, fd, (long)buf, len, flags, (long)addr, alen);
}

ssize_t sendmsg(int fd, const void *msg, int flags) {
	/* msg: {name, namelen, iov, iovlen, ...} — name at offset 0 */
	const unsigned char *m = msg;
	const unsigned char *name = *(const unsigned char **)m;
	unsigned namelen = *(unsigned *)(m+8);
	const unsigned char *iov = *(const unsigned char **)(m+16);
	unsigned iovlen = *(unsigned *)(m+24);
	if (name && namelen >= 8) {
		unsigned fam = name[0] | (name[1]<<8);
		if (fam == AF_QIPCRTR && iov && iovlen) {
			unsigned node = name[4]|(name[5]<<8)|(name[6]<<16)|(name[7]<<24);
			unsigned port = name[8]|(name[9]<<8)|(name[10]<<16)|(name[11]<<24);
			const unsigned char *base = *(const unsigned char **)iov;
			unsigned blen = *(unsigned *)(iov+8);
			cap_open(); logline('S', node, port, base, blen);
		}
	}
	return (ssize_t)__sys4(__NR_sendmsg, fd, (long)msg, flags, 0);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, void *addr, unsigned *alen) {
	ssize_t r = (ssize_t)__sys6(__NR_recvfrom, fd, (long)buf, len, flags, (long)addr, (long)alen);
	if (r > 0 && addr && alen && *alen >= 8) {
		const unsigned char *a = addr;
		unsigned fam = a[0] | (a[1]<<8);
		if (fam == AF_QIPCRTR) {
			unsigned node = a[4]|(a[5]<<8)|(a[6]<<16)|(a[7]<<24);
			unsigned port = a[8]|(a[9]<<8)|(a[10]<<16)|(a[11]<<24);
			cap_open(); logline('R', node, port, buf, (size_t)r);
		}
	}
	return r;
}

ssize_t recvmsg(int fd, void *msg, int flags) {
	const unsigned char *m = msg;
	ssize_t r = (ssize_t)__sys4(__NR_recvmsg, fd, (long)msg, flags, 0);
	if (r > 0) {
		const unsigned char *name = *(const unsigned char **)m;
		unsigned namelen = *(unsigned *)(m+8);
		const unsigned char *iov = *(const unsigned char **)(m+16);
		unsigned iovlen = *(unsigned *)(m+24);
		if (name && namelen >= 8) {
			unsigned fam = name[0] | (name[1]<<8);
			if (fam == AF_QIPCRTR && iov && iovlen) {
				unsigned node = name[4]|(name[5]<<8)|(name[6]<<16)|(name[7]<<24);
				unsigned port = name[8]|(name[9]<<8)|(name[10]<<16)|(name[11]<<24);
				const unsigned char *base = *(const unsigned char **)iov;
				cap_open(); logline('R', node, port, base, (size_t)r);
			}
		}
	}
	return r;
}

/* LD_PRELOAD entry: nothing needed at load; first QRTR op opens the log. */
