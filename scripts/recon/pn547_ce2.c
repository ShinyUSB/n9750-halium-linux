/* pn547_ce2 — PN553 (NQ220) full stock-equivalent NCI init + NFC-A Type-4A card emulation on Debian.
 *
 * Sequence reconstructed from:
 *   - NXP NFC_NCIHAL_libnfc-nci (pn5xx) halimpl: phNxpNciHal.cc phNxpNciHal_MinOpen() /
 *     phNxpNciHal_core_initialized() (init order), phNfcCommon.h (PLL/DPLL constants),
 *     phNxpNciHal_ext.cc (FW version decode: last 3 bytes of CORE_INIT_RSP = rom/maj/min)
 *   - Stock Samsung configs (SM-N9750 N9750ZSU6HZE1 vendor.img):
 *       /vendor/etc/libnfc-nxp.conf      (ACT_PROP, PROFILE_EXTN, CORE_STANDBY, MFCKEY, SWP...)
 *       /vendor/etc/nfc/libnfc-nxp_RF.conf (RF_CONF_BLK_1..4, CORE_CONF_EXTN, CORE_CONF)
 *   - NCI 1.0 stack (libnfc-nci): nci_defs.h OIDs, nci_hmsgs.cc nci_snd_set_routing_cmd
 *     payload format [more][num_tlv][TLVs], nfa_ee_act.cc add_route_tech_proto_tlv
 *     TLV = [tag][len=3][nfcee_id][power][tech], NCI_RF_TECHNOLOGY_A=0x00.
 *
 * ROOT CAUSE of the previous CE failure (pn547_ce.c): it sent 21 02 (RF_GET_ROUTING, a
 * read-only query) instead of 21 01 (RF_SET_LISTEN_MODE_ROUTING) and used tech byte 0x80
 * (listen-A discovery type) instead of 0x00 (NFC-A RF technology). No DH listen route was
 * ever installed => NFCC never delivered RF_INTF_ACTIVATED to the host.
 *
 * Modes:
 *   c  card emulation (default): full init + listen NFC-A -> DH + Type-4A NDEF responder
 *   i  init + config readback only (verify what persists in the chip)
 *   r  reader/poll mode (regression: must still detect real tags)
 *
 * Build (host):
 *   clang --target=aarch64-linux-android -nostdlib -fuse-ld=lld -fno-stack-protector \
 *         -static -O2 pn547_ce2.c -o pn547_ce2
 * Run (phone): /root/pn547_ce2 c   (use `timeout N` — runs until killed)
 */
typedef unsigned long size_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

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
#define SYS_clock_gettime 113
#define O_RDWR 2
#define O_NONBLOCK 02000

static int openat(const char *p, int fl) { return sys3(SYS_openat, -100, (long)p, fl); }
static long read_(int fd, void *b, long n) { return sys3(SYS_read, fd, (long)b, n); }
static long write_(int fd, const void *b, long n) { return sys3(SYS_write, fd, (long)b, n); }
static long ioctl_(int fd, long req, long arg) { return sys3(SYS_ioctl, fd, req, arg); }
static void msleep(int ms) { struct { long s, ns; } ts = { ms/1000, (long)(ms%1000)*1000000L }; sys4(SYS_nanosleep, (long)&ts, 0, 0, 0); }
static long now_ms(void) {
    struct { long s, ns; } ts;
    sys3(SYS_clock_gettime, 1 /*CLOCK_MONOTONIC*/, (long)&ts, 0);
    return ts.s * 1000 + ts.ns / 1000000;
}
static void puts_(const char *s) { long n=0; while(s[n]) n++; write_(1, s, n); }
static void puthex(unsigned char b) { const char *h="0123456789abcdef"; char c[3]={h[b>>4],h[b&15],' '}; write_(1,c,3); }
static void pnum(long v) { char b[24]; int i=23; b[i--]='\0'; if(!v) b[i--]='0'; while(v){b[i--]='0'+v%10;v/=10;} write_(1,b+i+1,23-i); }
static void dump(const char *tag, const unsigned char *b, long n) {
    puts_(tag); for (long i=0;i<n && i<64;i++) puthex(b[i]); puts_("\n");
}

#define PN547_SET_PWR 0x4008E901

/* ================= NCI 1.0 constants (nci_defs.h) ================= */
#define NCI_MT_DATA 0x00
#define NCI_MT_RSP  0x40
#define NCI_MT_NTF  0x60
#define GID_CORE 0
#define GID_RF   1
#define OID_CORE_RESET   0
#define OID_CORE_INIT    1
#define OID_CORE_SETCFG  2
#define OID_CORE_GETCFG  3
#define OID_CORE_CONNCREATE 4
#define OID_RF_DISC_MAP   0
#define OID_RF_SET_ROUTING 1
#define OID_RF_GET_ROUTING 2
#define OID_RF_DISCOVER   3
#define NTF_INTF_ACTIVATED  (0x61*256+0x05)
#define NTF_DEACTIVATED     (0x61*256+0x06)
#define NTF_FIELD_INFO      (0x61*256+0x08)
#define IFACE_ISO_DEP 0x02
#define PROTO_ISO_DEP 0x04
#define RF_TECH_A     0x00
#define DISC_LISTEN_A_PASSIVE 0x80
#define NCI_DH_ID 0x00

/* ================= NXP proprietary init sequence =================
 * Order per phNxpNciHal.cc MinOpen + core_initialized + stock confs.
 * The big CORE_SET_CONFIG blocks are GENERATED from the stock Samsung config
 * files by conf2c.py (see nfc_cfg.h) — no hand transcription. */
#include "nfc_cfg.h"

/* phNxpNciHal_core_initialized: NXP_ACT_PROP_EXTN (libnfc-nxp.conf) */
static const unsigned char c_act_prop[]   = {0x2F, 0x02, 0x00};
/* NAME_NXP_CORE_STANDBY (libnfc-nxp.conf) */
static const unsigned char c_standby[]    = {0x2F, 0x00, 0x01, 0x01};
/* cmd_ven_pulld_enable_nci (phNxpNciHal.cc:1610) */
static const unsigned char c_ven_pulld[]  = {0x20, 0x02, 0x05, 0x01, 0xA0, 0x07, 0x01, 0x03};
/* NAME_NXP_NFC_PROFILE_EXTN (libnfc-nxp.conf) - EMVCo off / Nfc Forum mode */
static const unsigned char c_profile[]    = {0x20, 0x02, 0x05, 0x01, 0xA0, 0x44, 0x01, 0x00};
/* phNxpNciHal_txNfccClockSetCmd pn553/PLL: A0 03 = 0 (freq set via PLL regs) */
static const unsigned char c_clk_set[]    = {0x20, 0x02, 0x05, 0x01, 0xA0, 0x03, 0x01, 0x00};
/* SET_CONFIG_CMD_PLL_19_2MHZ (phNfcCommon.h:88) */
static const unsigned char c_pll_192[]    = {0x20, 0x02, 0x0C, 0x01, 0xA0, 0x20, 0x08, 0x88,
                                              0x51, 0xE3, 0x02, 0xB8, 0x21, 0xE1, 0x02};
/* SET_CONFIG_CMD_DPLL_19_2MHZ (phNfcCommon.h:93) */
static const unsigned char c_dpll_192[]   = {0x20, 0x02, 0x0C, 0x01, 0xA0, 0x26, 0x08, 0x88,
                                              0x01, 0xE2, 0x02, 0xF0, 0x00, 0xA2, 0x01};
/* NXP_SWP_SWITCH_TIMEOUT=0x0A -> 10000 ms (phNxpNciHal.cc swp_switch_timeout_cmd) */
static const unsigned char c_swp_to[] = {0x20,0x02,0x06,0x01,0xA0,0xF3,0x02,0x10,0x27};
/* NXP_SWP_FULL_PWR_ON=0x00 -> off (phNxpNciHal.cc swp_full_pwr_mode_on_cmd) */
static const unsigned char c_swp_fp[] = {0x20,0x02,0x05,0x01,0xA0,0xF1,0x01,0x00};
/* screen state: 2F 15 01 00 = SCREEN ON UNLOCKED (phNxpNciHal.cc set_screen_state) */
static const unsigned char c_screen_on[] = {0x2F,0x15,0x01,0x00};
/* CORE_CONN_CREATE static RF connection (phNxpNciHal.cc nfcc_core_conn_create) */
static const unsigned char c_conn_create[] = {0x20,0x04,0x06,0x03,0x01,0x01,0x02,0x01,0x01};
/* RF_DISCOVER_MAP: ISO-DEP (0x04) LISTEN (0x02) -> ISO-DEP interface (0x02) */
static const unsigned char c_disc_map[] = {0x21,0x00,0x04,0x01,0x04,0x02,0x02};
/* RF_SET_LISTEN_MODE_ROUTING (21 01!): more=0, num=1, TLV tech-route
 * [0x00 tag][0x03 len][DH=0x00][pwr switch-on=0x01][NFC-A=0x00]  — THE FIX */
static const unsigned char c_route_a[] = {0x21,0x01,0x07,0x00,0x01,0x00,0x03,0x00,0x01,0x00};
/* RF_GET_ROUTING (21 02) readback */
static const unsigned char c_route_get[] = {0x21,0x02,0x00};
/* RF_DISCOVER: listen NFC-A passive (0x80), freq 1 */
static const unsigned char c_discover_listen[] = {0x21,0x03,0x03,0x01,0x80,0x01};
/* RF_DISCOVER: poll A/B/F (regression mode) */
static const unsigned char c_discover_poll[] = {0x21,0x03,0x07,0x03,0x00,0x01,0x01,0x01,0x02,0x01};

/* ================= Type-4A tag data ================= */
/* NDEF message served after a reader writes one; starts EMPTY (NLEN=0)
 * so write-mode apps see an empty, NDEF-formatted tag. */
static unsigned char ndef_file[2 + 255];   /* NLEN + NDEF message */
static unsigned int  ndef_len = 0;         /* NLEN value */

static unsigned char cc_file[] = {
    0x00,0x0F,        /* CCLEN = 15 */
    0x20,             /* mapping version 2.0 */
    0x00,0xF9,        /* MLe = 249 */
    0x00,0xF6,        /* MLc = 246 */
    0x04,0x06,        /* NDEF File Control TLV */
    0xE1,0x04,        /* NDEF file id */
    0x00,0xFF,        /* max NDEF size 255 */
    0x00,             /* read access open */
    0x00              /* write access open */
};

/* ================= transport / framing ================= */
static void *memcpy_(void *d, const void *s, unsigned long n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
static long strlen_(const char *s) { long n = 0; while (s[n]) n++; return n; }
void *memcpy(void *d, const void *s, unsigned long n) { return memcpy_(d, s, n); }
void *memset(void *d, int c, unsigned long n) {
    unsigned char *dd = d; while (n--) *dd++ = (unsigned char)c; return d;
}
unsigned long strlen(const char *s) { return (unsigned long)strlen_(s); }

static int fd;
static unsigned char rbuf[2048];   /* accumulated driver bytes */
static long rlen = 0;
static long tx_count, rx_count, ntf_count, act_count, apdu_count;
static long t_start;

static void tstamp(void) { pnum(now_ms() - t_start); puts_("ms "); }

static void log_rx(const unsigned char *b, long n) {
    rx_count++;
    tstamp(); puts_("RX  "); dump("", b, n);
}
static void log_tx(const unsigned char *b, long n) {
    tx_count++;
    tstamp(); puts_("TX> "); dump("", b, n);
}

/* read more bytes from driver into rbuf; returns bytes added (>=0).
 * 260 = NCI_MAX_DATA_LEN, same as the vendor HAL's phTmlNfc read size.
 * (Larger reads wedge the GENI I2C controller after the chip NACKs.) */
static long pump(void) {
    unsigned char tmp[260];
    long n = read_(fd, tmp, sizeof(tmp));
    if (n <= 0) return 0;
    if (rlen + n > (long)sizeof(rbuf)) { rlen = 0; }  /* overflow guard */
    for (long i = 0; i < n; i++) rbuf[rlen++] = tmp[i];
    return n;
}

static void drop1(void) {
    for (long i = 1; i < rlen; i++) rbuf[i - 1] = rbuf[i];
    rlen--;
}

/* extract one complete NCI packet from rbuf; returns length or 0 (none yet).
 * NOTE: i2c_master_recv(count) returns count bytes — the chip releases the bus
 * after its packet and the controller pads the tail with 0xff. The NCI layer
 * frames by header length and must RESYNC past the padding (drop-one-byte). */
static long pop_packet(unsigned char *out) {
    for (;;) {
        if (rlen < 3) return 0;
        unsigned char mt = rbuf[0] & 0xE0;
        long plen;
        if (mt == NCI_MT_DATA) {
            plen = ((long)rbuf[1] << 8) | rbuf[2];
            if (plen > (long)sizeof(rbuf) - 3) { drop1(); continue; }
        } else if (mt == NCI_MT_RSP || mt == NCI_MT_NTF) {
            plen = rbuf[2];
            if (plen > 252) { drop1(); continue; }
        } else {
            drop1();   /* 0xff padding or resync junk */
            continue;
        }
        long total = 3 + plen;
        if (rlen < total) return 0;
        for (long i = 0; i < total; i++) out[i] = rbuf[i];
        for (long i = total; i < rlen; i++) rbuf[i - total] = rbuf[i];
        rlen -= total;
        return total;
    }
}

/* send cmd, wait for matching RSP (gid,oid); returns rsp payload len (>=0), rsp bytes in *rsp
 * (rsp = full packet incl header). Any NTF seen while waiting is logged. 0 = timeout. */
static long send_rsp(const unsigned char *cmd, long clen, unsigned char *rsp, long rsmax) {
    unsigned char pkt[600];
    long deadline = now_ms() + 600;
    log_tx(cmd, clen);
    if (write_(fd, cmd, clen) != clen) { puts_("[TX FAILED]\n"); return -1; }
    while (now_ms() < deadline) {
        long n = pop_packet(pkt);
        if (n == 0) { pump(); msleep(5); continue; }
        unsigned char mt = pkt[0] & 0xE0;
        if (mt == NCI_MT_RSP && (pkt[0] & 0x1F) == (cmd[0] & 0x1F) && pkt[1] == cmd[1]) {
            log_rx(pkt, n);
            long c = n < rsmax ? n : rsmax;
            for (long i = 0; i < c; i++) rsp[i] = pkt[i];
            return n;
        }
        log_rx(pkt, n);
        ntf_count++;
    }
    puts_("[TIMEOUT waiting for RSP]\n");
    return 0;
}

/* send cmd, check status byte (rsp[3]); returns 1 if status 0x00 */
static int send_ok(const char *name, const unsigned char *cmd, long clen) {
    unsigned char rsp[600];
    long n = send_rsp(cmd, clen, rsp, sizeof(rsp));
    if (n >= 4 && rsp[3] == 0x00) return 1;
    puts_("["); puts_(name); puts_("] BAD/timeout — status: ");
    if (n >= 4) puthex(rsp[3]); else puts_("none");
    puts_("\n");
    return 0;
}

/* ================= config readback ================= */
/* payload = [num][ids...]: 1-octet IDs for standard params (30..33),
 * 2-octet IDs for NXP proprietary (A0 xx) — both forms verified on device. */
static void get_cfg_raw(const unsigned char *payload, long plen) {
    unsigned char cmd[260];
    unsigned char rsp[600];
    long n = 0;
    cmd[n++] = 0x20; cmd[n++] = OID_CORE_GETCFG;
    cmd[n++] = (unsigned char)plen;
    for (long i = 0; i < plen; i++) cmd[n++] = payload[i];
    long rn = send_rsp(cmd, n, rsp, sizeof(rsp));
    if (rn < 5 || rsp[3] != 0x00) { puts_("[GET_CONFIG bad status]\n"); return; }
    puts_("CFG< ");
    for (long i = 4; i < rn; i++) puthex(rsp[i]);
    puts_("\n");
}

/* ================= init ================= */
static int do_init(void) {
    unsigned char rsp[600];
    long n;

    ioctl_(fd, PN547_SET_PWR, 1);
    msleep(150);

    /* 1. CORE_RESET (type 0x00 = reset config — same as HAL MinOpen).
     * (PN553 NCI1.0 sends no CORE_RESET_NTF after this — verified on device.) */
    static const unsigned char reset[] = {0x20, 0x00, 0x01, 0x00};
    if (!send_ok("CORE_RESET", reset, sizeof(reset))) return 0;

    /* 2. CORE_INIT */
    static const unsigned char init[] = {0x20, 0x01, 0x00};
    n = send_rsp(init, sizeof(init), rsp, sizeof(rsp));
    if (n < 5 || rsp[3] != 0x00) { puts_("[CORE_INIT FAILED]\n"); return 0; }
    { /* FW version: last 3 payload bytes = rom/maj/min (phNxpNciHal_ext.cc:430-437) */
      long len = rsp[2] + 2;
      puts_("NFCC: rom_version="); puthex(rsp[len-2]);
      puts_(" fw_maj="); puthex(rsp[len-1]);
      puts_(" fw_min="); puthex(rsp[len]);
      puts_("\n"); }

    /* 3-5: NXP proprietary */
    send_ok("ACT_PROP",    c_act_prop,   sizeof(c_act_prop));
    send_ok("STANDBY",     c_standby,    sizeof(c_standby));
    send_ok("VEN_PULLD",   c_ven_pulld,  sizeof(c_ven_pulld));

    /* 6. clock: GET A0 02/03/04; set PLL@19.2 if A0 03 != 0x11 or A0 04 != 0x06
     * (phNxpNciHal_nfccClockCfgApply + determineConfiguredClockSrc, pn553 branch) */
    static const unsigned char getclk[] = {0x20, 0x03, 0x07, 0x03, 0xA0, 0x02, 0xA0, 0x03, 0xA0, 0x04};
    n = send_rsp(getclk, sizeof(getclk), rsp, sizeof(rsp));
    if (n >= 17 && rsp[3] == 0x00) {
        unsigned char a03 = rsp[12], a04 = rsp[16];
        puts_("clock: A0_03="); puthex(a03); puts_(" A0_04="); puthex(a04); puts_("\n");
        if (a03 != 0x11 || a04 != 0x06) {
            puts_("clock mismatch -> applying PLL@19.2MHz\n");
            send_ok("CLK SET", c_clk_set,  sizeof(c_clk_set));
            send_ok("PLL 19.2", c_pll_192, sizeof(c_pll_192));
            send_ok("DPLL 19.2", c_dpll_192, sizeof(c_dpll_192));
        }
    } else puts_("[clock GET_CONFIG failed — skipping clock set]\n");

    /* 7. PROFILE_EXTN (A0 44 = 0x00: EMVCo off / NFC Forum mode) */
    send_ok("PROFILE", c_profile, sizeof(c_profile));

    /* 8-10: RF blocks + core confs (phNxpNciHal.cc:1985-2040 order).
     * NOTE: RF BLK 3 (DLMA) returned STATUS_SYNTAX_ERROR (0x05) with the
     * FW11.01.1D-era table on FW 11.01.21 — the HAL would retry it; we log
     * and continue (same as HAL retry-exhausted behavior). */
    send_ok("RF BLK 1", c_rf_conf_blk_1, sizeof(c_rf_conf_blk_1));
    send_ok("RF BLK 2", c_rf_conf_blk_2, sizeof(c_rf_conf_blk_2));
    send_ok("RF BLK 3", c_rf_conf_blk_3, sizeof(c_rf_conf_blk_3));
    send_ok("RF BLK 4", c_rf_conf_blk_4, sizeof(c_rf_conf_blk_4));
    send_ok("CORE CONF EXT", c_core_conf_extn, sizeof(c_core_conf_extn));
    send_ok("CORE CONF", c_core_conf, sizeof(c_core_conf));

    /* 11-13: MFCKEY + SWP (phNxpNciHal.cc:2102+ / 2194).
     * (A0 91 AID-match dropped: STATUS_SYNTAX_ERROR on this FW — gated by the
     * stock HAL's feature flags.) */
    send_ok("MFCKEY", c_core_mfckey_setting, sizeof(c_core_mfckey_setting));
    send_ok("SWP TIMEOUT", c_swp_to, sizeof(c_swp_to));
    send_ok("SWP FULLPWR", c_swp_fp, sizeof(c_swp_fp));

    /* 15. screen state ON UNLOCKED (extns send this before listen) */
    send_ok("SCREEN", c_screen_on, sizeof(c_screen_on));

    /* 16. readbacks (verify chip state after config) */
    puts_("=== readback: LA params ===\n");
    { static const unsigned char p[] = {0x04, 0x30, 0x31, 0x32, 0x33};
      get_cfg_raw(p, 5); }
    puts_("=== readback: A0 44 (EMVCo), A0 40 (tagdet) ===\n");
    { static const unsigned char p[] = {0x02, 0xA0, 0x44, 0xA0, 0x40};
      get_cfg_raw(p, 5); }
    puts_("=== readback: clock A0 02/03/04 ===\n");
    { static const unsigned char p[] = {0x03, 0xA0, 0x02, 0xA0, 0x03, 0xA0, 0x04};
      get_cfg_raw(p, 7); }
    return 1;
}

/* ================= CE setup + listen loop ================= */
static void run_ce(void) {
    unsigned char pkt[600];

    if (!do_init()) return;

    /* static RF connection */
    send_ok("CONN CREATE", c_conn_create, sizeof(c_conn_create));

    /* discovery map: ISO-DEP listen -> ISO-DEP iface */
    send_ok("DISC MAP", c_disc_map, sizeof(c_disc_map));

    /* *** THE FIX: RF_SET_LISTEN_MODE_ROUTING (21 01), tech route NFC-A -> DH *** */
    send_ok("SET ROUTE", c_route_a, sizeof(c_route_a));

    /* readback routing table (proof it stuck) */
    { unsigned char rsp[600];
      long n = send_rsp(c_route_get, sizeof(c_route_get), rsp, sizeof(rsp));
      if (n >= 4 && rsp[3] == 0x00) {
          puts_("ROUTING TABLE: more="); puthex(rsp[4]); puts_(" num="); puthex(rsp[5]);
          puts_(" TLVs: "); for (long i = 6; i < n; i++) puthex(rsp[i]); puts_("\n");
      } else puts_("[GET ROUTE failed]\n"); }

    /* discover: listen NFC-A passive */
    send_ok("DISCOVER", c_discover_listen, sizeof(c_discover_listen));

    puts_("=== CE listening (Type-4A, NDEF empty). Hold the other phone near. ===\n");
    puts_("=== Ctrl: killall pn547_ce2. Log: every NCI packet. ===\n");

    ndef_file[0] = 0; ndef_file[1] = 0;   /* NLEN = 0 (empty) */
    ndef_len = 0;
    int fid_sel = 0;                      /* 0 none, 1 CC (E103), 2 NDEF (E104) */

    int conn = -1;
    long deadline = now_ms() + 600000;    /* 10 min hard cap */
    while (now_ms() < deadline) {
        long n = pop_packet(pkt);
        if (n == 0) { pump(); msleep(10); continue; }
        unsigned char mt = pkt[0] & 0xE0;
        if (mt == NCI_MT_NTF) {
            log_rx(pkt, n);
            unsigned short id = (unsigned short)(pkt[0] * 256 + pkt[1]);
            if (id == NTF_INTF_ACTIVATED) {
                act_count++;
                conn = pkt[3] & 0x0F;
                puts_("*** RF_INTF_ACTIVATED: iface="); puthex(pkt[4]);
                puts_(" proto="); puthex(pkt[5]);
                puts_(" mode/tech="); puthex(pkt[6]);
                puts_(" conn="); puthex(conn); puts_(" ***\n");
            } else if (id == NTF_DEACTIVATED) {
                puts_("*** RF_DEACTIVATED (reader gone) ***\n");
                conn = -1;
            } else if (id == NTF_FIELD_INFO) {
                puts_("RF_FIELD: "); puthex(pkt[3]); puts_("\n");
            } else {
                ntf_count++;
            }
            continue;
        }
        if (mt != NCI_MT_DATA) continue;

        apdu_count++;
        log_rx(pkt, n);
        long alen = n - 3;
        if (alen < 4) { goto nack; }
        unsigned char cid = pkt[0] & 0x0F;
        const unsigned char *apdu = pkt + 3;

        /* build response: [cid][len_hi][len_lo] + body */
        unsigned char out[600];
        long blen = 0;
        unsigned char cla = apdu[0], ins = apdu[1];
        if (cla == 0x00 && ins == 0xA4) {
            /* SELECT */
            if (alen >= 7 && apdu[2] == 0x00 && (apdu[3] & 0x0C) == 0x0C) {
                unsigned int fid = ((unsigned)apdu[5] << 8) | apdu[6];
                if (fid == 0xE103) { fid_sel = 1; out[3] = 0x90; out[4] = 0x00; blen = 2; }
                else if (fid == 0xE104) { fid_sel = 2; out[3] = 0x90; out[4] = 0x00; blen = 2; }
                else { fid_sel = 0; out[3] = 0x6A; out[4] = 0x82; blen = 2; }
            } else { out[3] = 0x6A; out[4] = 0x86; blen = 2; }
        } else if (cla == 0x00 && ins == 0xB0) {
            /* READ BINARY */
            unsigned int off = ((unsigned)apdu[2] << 8) | apdu[3];
            unsigned int le = apdu[4] ? apdu[4] : 256;
            const unsigned char *src; long slen;
            if (fid_sel == 1) { src = cc_file; slen = sizeof(cc_file); }
            else { ndef_file[0] = (unsigned char)(ndef_len >> 8);
                   ndef_file[1] = (unsigned char)(ndef_len & 0xFF);
                   src = ndef_file; slen = 2 + (long)ndef_len; }
            if (fid_sel == 0) { out[3] = 0x6A; out[4] = 0x82; blen = 2; }
            else if (off < (unsigned)slen) {
                long avail = slen - (long)off;
                long cpy = (long)le < avail ? (long)le : avail;
                if (cpy > 255) cpy = 255;
                for (long i = 0; i < cpy; i++) out[3+i] = src[off+i];
                out[3+cpy] = 0x90; out[4+cpy] = 0x00; blen = cpy + 2;
            } else { out[3] = 0x6A; out[4] = 0x82; blen = 2; }
        } else if (cla == 0x00 && ins == 0xD6) {
            /* UPDATE BINARY — accept writes to the NDEF file */
            unsigned int off = ((unsigned)apdu[2] << 8) | apdu[3];
            unsigned int lc = apdu[4];
            if (fid_sel == 2 && off < 250) {
                if (off == 0 && lc >= 2) {
                    ndef_len = ((unsigned)apdu[5] << 8) | apdu[6];
                }
                out[3] = 0x90; out[4] = 0x00; blen = 2;
                puts_("[NDEF write: NLEN now "); pnum(ndef_len); puts_("]\n");
            } else { out[3] = 0x6A; out[4] = 0x82; blen = 2; }
        } else {
            out[3] = 0x6D; out[4] = 0x00; blen = 2;
        }
        {
            long total = 3 + blen;
            out[0] = cid;
            out[1] = (unsigned char)((blen >> 8) & 0xFF);
            out[2] = (unsigned char)(blen & 0xFF);
            log_tx(out, total);
            write_(fd, out, total);
        }
        continue;
    nack:
        { unsigned char out[6]; out[0] = pkt[0] & 0x0F; out[1] = 0; out[2] = 2;
          out[3] = 0x6F; out[4] = 0x00; log_tx(out, 5); write_(fd, out, 5); }
    }
    puts_("=== done: tx="); pnum(tx_count); puts_(" rx="); pnum(rx_count);
    puts_(" ntf="); pnum(ntf_count); puts_(" activations="); pnum(act_count);
    puts_(" apdus="); pnum(apdu_count); puts_("\n");
}

/* ================= reader regression mode ================= */
static void run_reader(void) {
    unsigned char pkt[600];
    if (!do_init()) return;
    send_ok("CONN CREATE", c_conn_create, sizeof(c_conn_create));
    send_ok("DISC MAP P", c_disc_map, sizeof(c_disc_map));
    send_ok("DISCOVER P", c_discover_poll, sizeof(c_discover_poll));
    puts_("=== reader mode: present a tag ===\n");
    long deadline = now_ms() + 60000;
    while (now_ms() < deadline) {
        long n = pop_packet(pkt);
        if (n == 0) { pump(); msleep(10); continue; }
        unsigned char mt = pkt[0] & 0xE0;
        if (mt == NCI_MT_NTF) {
            log_rx(pkt, n);
            if ((unsigned short)(pkt[0]*256+pkt[1]) == NTF_INTF_ACTIVATED) {
                puts_("*** TAG DETECTED: proto="); puthex(pkt[5]); puts_(" ***\n");
            }
        }
    }
}

int main(int argc, char **argv) {
    char mode = 'c';
    if (argc > 1) mode = argv[1][0];

    t_start = now_ms();
    fd = openat("/dev/pn547", O_RDWR | O_NONBLOCK);
    if (fd < 0) { puts_("open /dev/pn547 FAILED (EBUSY? killall pn547_ce2 pn547_test pn547_poll pn547_listen)\n"); return 1; }
    puts_("pn547_ce2: open ok, mode="); write_(1, &mode, 1); puts_("\n");

    if (mode == 'r') run_reader();
    else if (mode == 'i') { do_init(); ioctl_(fd, PN547_SET_PWR, 0); }
    else run_ce();

    puts_("done\n");
    return 0;
}

/* raw entry: x0 = sp (kernel places argc/argv at sp) — must run before any
 * C prologue, so it is a naked asm stub jumping into a C function. */
__asm__(
    ".global _start\n"
    "_start:\n"
    "   mov x0, sp\n"
    "   b c_entry\n"
);
void c_entry(long spv) {
    long argc = *(long *)spv;
    char **argv = (char **)(spv + 8);
    main((int)argc, argv);
    sys3(93, 0, 0, 0);   /* exit(0) */
}
