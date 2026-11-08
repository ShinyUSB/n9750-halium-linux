/* pn547 card-emulation (HCE) — emulate a Type-4A NDEF tag readable by another phone.
 * Raw NCI over /dev/pn547. Freestanding (raw aarch64 syscalls), no libc. */
typedef unsigned long size_t;

static inline long sys3(long n, long a, long b, long c) {
    register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
    register long x8 asm("x8") = n;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}
static inline long sys4(long n, long a, long b, long c, long d) {
    register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d;
    register long x8 asm("x8") = n;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}
#define SYS_openat 56
#define SYS_read   63
#define SYS_write  64
#define SYS_close  57
#define SYS_ioctl  29
#define SYS_nanosleep 101
#define O_RDWR 2

static int openat(const char *p, int fl) { return sys3(SYS_openat, -100, (long)p, fl); }
static long read_(int fd, void *b, long n) { return sys3(SYS_read, fd, (long)b, n); }
static long write_(int fd, const void *b, long n) { return sys3(SYS_write, fd, (long)b, n); }
static long ioctl_(int fd, long req, long arg) { return sys3(SYS_ioctl, fd, req, arg); }
static void msleep(int ms) { struct { long s, ns; } ts = { ms/1000, (long)(ms%1000)*1000000L }; sys4(SYS_nanosleep, (long)&ts, 0, 0, 0); }
static void puts_(const char *s) { long n=0; while(s[n]) n++; write_(1, s, n); }
static void puthex(unsigned char b) { const char *h="0123456789abcdef"; char c[3]={h[b>>4],h[b&15],' '}; write_(1,c,3); }
static void dump(const char *tag, unsigned char *b, long n) {
    puts_(tag); if (n > 0) for (long i=0;i<n && i<48;i++) puthex(b[i]); puts_("\n");
}

#define PN547_SET_PWR 0x4008E901

/* ---- Type-4A tag data ---- */
/* NDEF message: a single Text record "d2q NFC" (en) */
static unsigned char ndef_msg[] = {
    0xD1, 0x01, 0x08, 0x54, 0x02, 'e','n', 'd','2','q',' ','N','F','C'
};
#define NDEF_LEN (sizeof(ndef_msg))

/* Capability Container (Type-4A, mapping v2.0) */
static unsigned char cc_file[] = {
    0x00,0x0F,        /* CCLEN = 15 */
    0x20,             /* mapping version 2.0 */
    0x00,0xF9,        /* MLe  = 249 */
    0x00,0xF6,        /* MLc  = 246 */
    0x04,0x06,        /* NDEF File Control TLV: T=4 L=6 */
    0xE1,0x04,        /* NDEF file id = E104 */
    0x00,0x00,        /* max NDEF size hi (patched below) */
    0x00,0x00,        /*   ... lo */
    0x00,             /* read access: open */
    0x00              /* write access: open */
};

int main(void) {
    static const unsigned char core_reset[] = { 0x20, 0x00, 0x01, 0x01 };
    static const unsigned char core_init[]  = { 0x20, 0x01, 0x00 };
    /* RF_DISCOVER_MAP: ISO-DEP (0x04) in LISTEN mode (0x02) -> host ISO_DEP iface (0x02) */
    static const unsigned char rf_discover_map[] = { 0x21, 0x00, 0x04, 0x01, 0x04, 0x02, 0x02 };
    /* RF_DISCOVER: 1 config: Listen NFC-A passive (0x80), frequency 1 */
    static const unsigned char rf_discover[] = { 0x21, 0x03, 0x03, 0x01, 0x80, 0x01 };
    unsigned char buf[512];
    unsigned char resp[300];
    unsigned char ndef_file[2 + 64];      /* NLEN + ndef_msg */
    int cur_file = 0;                     /* 0=app, 1=CC, 2=NDEF */

    cc_file[10] = (unsigned char)(NDEF_LEN >> 8);
    cc_file[11] = (unsigned char)(NDEF_LEN & 0xFF);

    int fd = openat("/dev/pn547", O_RDWR);
    if (fd < 0) { puts_("open /dev/pn547 FAILED\n"); return 1; }
    puts_("pn547 CE: open ok\n");

    ioctl_(fd, PN547_SET_PWR, 1);
    msleep(150);
    write_(fd, core_reset, 4); msleep(120); read_(fd, buf, sizeof(buf));
    write_(fd, core_init, 3);  msleep(120); read_(fd, buf, sizeof(buf));
    /* CORE_SET_CONFIG: LA_SEL_INFO (0x32) = ISO_DEP (0x20) so the tag is a Type-4A tag */
    static const unsigned char set_cfg[] = { 0x20, 0x02, 0x04, 0x01, 0x32, 0x01, 0x20 };
    write_(fd, set_cfg, sizeof(set_cfg)); msleep(120); read_(fd, buf, sizeof(buf));
    /* CORE_SET_CONFIG: LA_NFCID1 (0x33) = a 4-byte UID for anti-collision */
    static const unsigned char set_uid[] = { 0x20, 0x02, 0x07, 0x01, 0x33, 0x04, 0x08, 0x44, 0x32, 0x51 };
    write_(fd, set_uid, sizeof(set_uid)); msleep(120); read_(fd, buf, sizeof(buf));
    write_(fd, rf_discover_map, sizeof(rf_discover_map)); msleep(120); read_(fd, buf, sizeof(buf));
    /* RF_SET_LISTEN_MODE_ROUTING: route listen NFC-A to the HOST (DH id 0), power on.
     * Entry = [0x00 tech-route][3][0x00 DH][0x01 on][0x80 listen A]. Without this the
     * listen traffic goes to the eSE by default and the host never sees it (HCE needs host). */
    static const unsigned char listen_route[] = { 0x21, 0x02, 0x07, 0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x80 };
    write_(fd, listen_route, sizeof(listen_route)); msleep(120); read_(fd, buf, sizeof(buf));
    write_(fd, rf_discover, sizeof(rf_discover)); msleep(120);
    long n = read_(fd, buf, sizeof(buf));
    dump("RF_DISCOVER rsp", buf, n);

    puts_("=== d2q is a Type-4A tag (text: 'd2q NFC'). Hold the other phone near. ===\n");

    int conn = -1;
    for (int it = 0; it < 1200; it++) {
        n = read_(fd, buf, sizeof(buf));
        if (n <= 0) { msleep(30); continue; }
        dump("RAW<", buf, n);   /* print EVERYTHING */
        unsigned char mt = buf[0] & 0xE0;   /* NCI MT: 0x00=DATA 0x40=RSP 0x60=NTF */
        if (mt == 0x60) {
            if (buf[0] == 0x61 && buf[1] == 0x05) {  /* RF_INTF_ACTIVATED */
                conn = buf[3];
                puts_("*** RF_INTF_ACTIVATED (reader connected) ***\n");
                dump("  act:", buf, n);
            } else if (buf[0] == 0x61 && buf[1] == 0x06) {
                puts_("*** RF_DEACTIVATED (reader left) ***\n");
                conn = -1;
            } else {
                dump("ntf:", buf, n);
            }
            continue;
        }
        if (mt != 0x00) continue;           /* only data messages below */

        unsigned char *apdu = buf + 1;
        long alen = n - 1;
        if (alen < 4) continue;
        dump("APDU<", apdu, alen);

        resp[0] = buf[0];
        long rlen = 0;
        unsigned char cla = apdu[0], ins = apdu[1];
        if (cla == 0x00 && ins == 0xA4) {
            if (alen >= 7 && apdu[2] == 0x00 && apdu[3] == 0x0C) {
                unsigned int fid = ((unsigned)apdu[5] << 8) | apdu[6];
                if (fid == 0xE103) cur_file = 1;
                else if (fid == 0xE104) cur_file = 2;
            }
            resp[1] = 0x90; resp[2] = 0x00; rlen = 2;
        } else if (cla == 0x00 && ins == 0xB0) {
            unsigned int off = ((unsigned)apdu[2] << 8) | apdu[3];
            unsigned int le = apdu[4] ? apdu[4] : 256;
            unsigned char *src; long slen;
            if (cur_file == 1) { src = cc_file; slen = sizeof(cc_file); }
            else {
                ndef_file[0] = (unsigned char)(NDEF_LEN >> 8);
                ndef_file[1] = (unsigned char)(NDEF_LEN & 0xFF);
                for (long i = 0; i < (long)NDEF_LEN; i++) ndef_file[2+i] = ndef_msg[i];
                src = ndef_file; slen = 2 + (long)NDEF_LEN;
            }
            if (off < (unsigned)slen) {
                long avail = slen - (long)off;
                long cpy = (long)le < avail ? (long)le : avail;
                for (long i = 0; i < cpy; i++) resp[1+i] = src[off + i];
                resp[1+cpy] = 0x90; resp[2+cpy] = 0x00; rlen = 1 + cpy + 2;
            } else {
                resp[1] = 0x6A; resp[2] = 0x82; rlen = 3;
            }
        } else {
            resp[1] = 0x6D; resp[2] = 0x00; rlen = 3;
        }

        write_(fd, resp, 1 + rlen);
        dump("RSP >", resp+1, rlen);
    }

    ioctl_(fd, PN547_SET_PWR, 0);
    puts_("done\n");
    return 0;
}

void _start(void) { main(); sys3(93, 0, 0, 0); }
