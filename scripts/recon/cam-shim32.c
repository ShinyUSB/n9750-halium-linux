// cam-shim32: standalone 32-bit bionic driver for Samsung's /vendor/lib/hw/camera.qcom.so
// Milestone 2a: open camera device, initialize, dump static metadata tags.
//
// Build:  clang --target=armv7a-linux-androideabi -mthumb -nostdlib -fuse-ld=lld \
//           -o cam-shim32 cam-shim32.c bionic32/libc.so bionic32/libdl.so \
//           bionic32/libcamera_metadata.so
// Run:    LD_LIBRARY_PATH=/vendor/lib:/opt/bionic32 /opt/bionic32/linker /usr/local/bin/cam-shim32

typedef unsigned int size_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

extern int printf(const char *, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern const char *dlerror(void);
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), const void *);
extern int setvbuf(void *stream, char *buf, int mode, size_t size);
extern void *stdout;
extern int gettid(void);
extern int clock_gettime(int, void *);
extern int putchar(int);
extern const char *get_camera_metadata_tag_name(uint32_t tag);
extern void *allocate_camera_metadata(size_t entry_capacity, size_t data_capacity);
extern int get_camera_metadata_tag_type(uint32_t tag);

/* ---- ion + buffer plumbing (4.14 legacy ion ABI) ---- */
extern int open(const char *, int, ...);
extern int ioctl(int, unsigned long, ...);
extern void *malloc(size_t);
extern void free(void *);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern void *mmap(void *, size_t, int, int, int, long);
extern int munmap(void *, size_t);
extern int close(int);

/* ShinyKernel uses the 4.19+ ion ABI: ALLOC returns the fd directly */
struct ion_alloc_414 { uint64_t len; uint32_t heap_mask; uint32_t flags; uint32_t fd; uint32_t unused; };
#define ION_IOC_ALLOC_414 0xc0184900uL  /* _IOWR('I',0,ion_allocation_data) */

/* native_handle_t (32-bit) */
typedef struct {
    int version; int numFds; int numInts;
    int data[0];
} native_handle_t;




/* camera3 stream ABI (32-bit) */
#ifdef __aarch64__
typedef struct camera3_stream {
    int stream_type;
    uint32_t width, height;
    int format;
    uint32_t usage;                 /* 0x10 — 64-bit usage is u64 in AOSP, Samsung uses u32 */
    uint32_t max_buffers;
    void *priv;                     /* 0x18 */
    int32_t data_space;             /* 0x20 */
    int32_t rotation;               /* 0x24 */
    const char *physical_camera_id; /* 0x28 (8B) */
    int32_t dynamic_range_profile;  /* 0x30 — Samsung streamOption! */
    int32_t samsung_stream_id;      /* 0x34 */
} camera3_stream_t;
#else
/* Samsung 32-bit vendor camera3_stream_t — VERIFIED from binary analysis
 * (device@5.0-impl 32-bit convertFromHidl + unihal CameraStream memcpy 0x68):
 * AOSP fields through 0x28, Samsung streamOption at 0x28 (AOSP reserved[0]),
 * Samsung extension 0x2c..0x67 (all zero in the real path), total 0x68. */
typedef struct camera3_stream {
    int stream_type;                 /* 0 = OUTPUT */
    uint32_t width, height;
    int format;                      /* HAL_PIXEL_FORMAT_RAW10 = 0x25 */
    uint32_t usage;
    uint32_t max_buffers;
    void *priv;
    int32_t data_space;
    int rotation;
    const char *physical_camera_id;  /* API 3.5 */
    int32_t dynamic_range_profile;   /* 0x28 — Samsung streamOption */
    uint32_t samsung_ext[15];        /* 0x2c..0x67 — must stay 0 */
} camera3_stream_t;
#endif

typedef struct camera3_stream_configuration {
    uint32_t num_streams;
    camera3_stream_t **streams;
    uint32_t operation_mode;
    const void *session_parameters;  /* camera_metadata_t*, may be 0 */
    int32_t input_width, input_height; /* API 3.5 */
} camera3_stream_configuration_t;

typedef struct camera3_stream_buffer {
    camera3_stream_t *stream;
    native_handle_t **buffer;        /* buffer_handle_t* */
    int status;
    int acquire_fence;
    int release_fence;
} camera3_stream_buffer_t;

/* Samsung vendor layout (verified from HAL disassembly): num_output_buffers
 * sits at offset 12, output_buffers array pointer at offset 16 — swapped vs AOSP. */
typedef struct camera3_capture_request {
    uint32_t frame_number;
    const void *settings;            /* camera_metadata_t* */
    camera3_stream_buffer_t *input_buffer;
    uint32_t num_output_buffers;
    const camera3_stream_buffer_t *output_buffers;
    int32_t input_width, input_height;
} camera3_capture_request_t;


/* persistent config storage (CamX keeps pointers into these) */
static camera3_stream_t g_st0, g_st1;
static camera3_stream_t *g_sarr[8];   /* 3-stream path uses index 2 — must fit */
static camera3_stream_configuration_t g_cfg;
static native_handle_t *g_handles[4];
static camera3_stream_buffer_t g_ob;
static native_handle_t **g_hslot;
static camera3_capture_request_t g_req;

/* ---- frame capture state ---- */
static volatile int got_frame = 0;
static void *frame_map[16];
static uint32_t frame_size[16];
static int frame_fd[16];

/* ---- lifecycle instrumentation ---- */
static int n_submitted = 0;      /* requests handed to process_capture_request */
static int n_results = 0;        /* process_capture_result callbacks */
static int n_buffers_back = 0;   /* results carrying >=1 output buffer */
static int n_notify = 0;         /* notify callbacks */
static int flush_called = 0, close_called = 0;
static void *build_session_params(int zoom_x100);  /* fwd */
static int set_or_update_tag(void *meta, uint32_t tag, const void *data, size_t count);
static uint32_t g_fmt0 = 0x23, g_fmt1 = 0x23, g_use0 = 0, g_use1 = 0;  /* stream formats/usages */
static const char *g_preview_physid = 0;   /* physical id for preview stream */
static int atoi_(const char *p) { int v = 0; while (*p >= '0' && *p <= '9') v = v*10 + (*p++ - '0'); return v; }

/* Dump every dmabuf this process owns (post-session introspection): scan
   /proc/self/fd/N for dmabuf symlinks, lseek for size, mmap, write to
   /tmp/dmabuf_<fd>_<size>.raw. Recovers the ToF raw-phase buffers that the
   SecCamTOF pipeline allocates internally (engine input buffers). */
static void dump_all_dmabufs(const char *tag) {
    extern long write(int, const void *, unsigned long);
    extern long lseek(int, long, int);
    extern long readlink(const char *, char *, unsigned long);
    int fd;
    for (fd = 3; fd < 512; fd++) {
        char lp[32]; int m = 0;
        const char *pf = "/proc/self/fd/"; while (*pf) lp[m++] = *pf++;
        int v = fd; char td[12]; int tc = 0; if (!v) td[tc++]='0'; while (v){td[tc++]='0'+v%10;v/=10;} while(tc) lp[m++]=td[--tc];
        lp[m] = 0;
        char tgt[128];
        long rl = readlink(lp, tgt, sizeof(tgt) - 1);
        if (rl <= 0) continue;
        tgt[rl] = 0;
        int isdm = 0;
        char *c;
        for (c = tgt; *c; c++) {
            if (c[0]=='d'&&c[1]=='m'&&c[2]=='a'&&c[3]=='b'&&c[4]=='u'&&c[5]=='f') { isdm = 1; break; }
        }
        if (!isdm) continue;
        long sz = lseek(fd, 0, 2 /*SEEK_END*/);
        lseek(fd, 0, 0);
        if (sz <= 4096 || sz >= 0x4000000) continue;
        void *p = mmap(0, sz, 3, 1 /*MAP_SHARED*/, fd, 0);
        if ((long)p == -1) continue;
        char op[96]; int q = 0;
        const char *pr = "/tmp/dmabuf_"; while (*pr) op[q++] = *pr++;
        const char *tg = tag; while (*tg) op[q++] = *tg++;
        op[q++] = '_';
        { int vv = fd, t2[12]; int t2c = 0; if (!vv) t2[t2c++]='0'; while (vv){t2[t2c++]='0'+vv%10;vv/=10;} while(t2c) op[q++]=t2[--t2c]; }
        op[q++] = '_';
        { long sv = sz; char t3[12]; int t3c = 0; if (!sv) t3[t3c++]='0'; while (sv){t3[t3c++]='0'+(int)(sv%10);sv/=10;} while(t3c) op[q++]=t3[--t3c]; }
        const char *sx = ".raw"; while (*sx) op[q++] = *sx++;
        op[q] = 0;
        int out = open(op, 0x241, 0666);
        if (out >= 0) { write(out, p, sz); close(out); }
        munmap(p, sz);
        printf("[dmadump] fd=%d sz=%ld -> %s\n", fd, sz, op);
    }
}

static int htoi_(const char *p) { int v = 0; if (p[0]=='0'&&(p[1]=='x'||p[1]=='X')) p += 2;
    while (*p) { int c=*p; int d=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; if(d<0)break; v=v*16+d; p++; } return v; }
static int g_argc = 0;             /* argv mirror for sub-mode helpers */
static char **g_argv = 0;
static int g_capture_phys = -1;   /* -1 = don't set capturePhysicalId */
static int g_zoom_x100 = -1;      /* zoom ratio * 100, -1 = don't set */
static int g_samsung_cam = -1;    /* samsungCamera marker */
static int g_capture_mode = -1;   /* samsung captureMode */
static int g_crop_x100 = -1;      /* scaler crop = active/zoom */
static int g_aperture = -1;       /* SS_AEC_APERTURECONTROL byte value */
static int g_ois_mode = -1;       /* OIS: 0=off 1=on (standard tag); samsung op mode = value */
static int g_lightcond = -1;      /* SS_AEC_LIGHTCONDITION override */
static int g_apex_x10 = -1;       /* SS_AEC_APEX float * 10 */

#define TAG_OIS_MODE        0x00080004   /* android.lens.opticalStabilizationMode (byte) */
#define TAG_SS_OIS_OPMODE   0x805d0000   /* samsung.android.lens.opticalStabilizationOperationMode (int32) */
static int g_req_delay_ms = 0;    /* delay between capture requests (test) */
static int g_dump_every = 0;      /* dump capture buffer every N requests (test) */
static const char *g_dump_prefix = "/data/local/tmp/slow";  /* dump path prefix */
static int g_vid_dump = 0;        /* dump video buffers in result callback */
static int g_tof_laser_on = 0;    /* ToF VCSEL fired this session */
static camera3_stream_t *g_vid_stream = &g_st1;  /* which stream to dump */
static int g_vid_seq = 0;
static int g_vid_seen = 0;
static int g_vid_step = 1;        /* dump every Nth video frame */
static uint32_t g_vid_width = 0, g_vid_height = 0;  /* dump dims (0 = use g_st1) */
static void *g_vid_stream2 = 0;                     /* optional 2nd dump stream */
static uint32_t g_vid_width2 = 0, g_vid_height2 = 0;
static int g_fps_range = 0;       /* 0 = don't set AE target fps */
static int g_sensor_mode = -1;    /* org.codeaurora.qcamera3.sensor_meta_data.sensor_mode_index */
static uint32_t g_prev_w = 1440, g_prev_h = 1080;  /* preview stream size */
static int g_recording_max_fps = -1;   /* samsung.android.control.recordingMaxFps */
static int g_recording_hint = -1;      /* samsung.android.control.recordingHint */
static int g_af_mode = -1;             /* android.control.afMode override */
static void *g_req_clone = 0;          /* per-session cloned+overridden request settings */
static int g_use_3rd = 0;              /* add YUV preview-callback stream (video usecase) */
static int g_poke_fps = 0;             /* poke CHI FPS global (0x2872EC) to this value */

static int hexv(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return 0; }
extern char *strstr(const char *, const char *);
extern long lseek(int, long, int);
extern long read(int, void *, unsigned long);

/* base address of the mapped chi override lib in OUR process */
static unsigned long get_chi_base(void) {
    char buf[256];
    int fd = open("/proc/self/maps", 0);
    if (fd < 0) return 0;
    unsigned long base = 0;
    unsigned long lo = 0, hi = 0;
    long n;
    /* read whole maps in chunks, find the chi line */
    (void)buf; (void)n; (void)lo; (void)hi;
    /* simple char reader */
    {
        char line[512];
        int li = 0;
        char c;
        /* rewind */
        lseek(fd, 0, 0);
        while (read(fd, &c, 1) == 1) {
            if (c == '\n') {
                line[li] = 0;
                /* does the line mention the chi lib after the dash part? */
                const char *nm = strstr(line, "com.samsung.chi.override.so");
                if (nm && lo == 0) {
                    /* parse start-end */
                    unsigned long a = 0, b = 0;
                    const char *q = line;
                    while (*q != '-') { a = a * 16 + hexv(*q); q++; }
                    q++; while (*q != ' ') { b = b * 16 + hexv(*q); q++; }
                    if (lo == 0 || a < lo) { lo = a; hi = b; }
                }
                li = 0;
            } else if (li < 500) {
                line[li++] = c;
            }
        }
    }
    close(fd);
    base = lo;
    return base;
}
static camera3_stream_t g_st3;
static native_handle_t *g_st3_handles[4];
static int g_st3_fd[4] = {-1,-1,-1,-1};
static int g_capture_mode_hint = -1;   /* samsung.android.control.captureMode (3=video) */
static int g_cap_buffers = 4;          /* rotating capture buffers (increase for high fps) */

#define TAG_LENS_APERTURE        0x00080000   /* android.lens.aperture (float, result) */
#define TAG_AVAIL_APERTURES      0x00090000   /* android.lens.info.availableApertures (float[]) */
#define TAG_SS_AEC_APERTURECTL   0x80580005   /* com.ss.stats.aec.SS_AEC_APERTURECONTROL (byte) */
#define TAG_SS_AEC_APERTURETEST  0x80580001   /* com.ss.stats.aec.SS_AEC_APERTURETEST (int32) */
#define TAG_SS_AEC_LIGHTCOND     0x80580004   /* com.ss.stats.aec.SS_AEC_LIGHTCONDITION (int32) */

static long long now_ms(void) {
    struct { long sec; long nsec; } ts;   /* 32-bit bionic timespec: 32-bit time_t */
    clock_gettime(1 /*CLOCK_MONOTONIC*/, &ts);
    return (long long)ts.sec * 1000 + ts.nsec / 1000000;
}
#define LLOG(tag, ...) \
    printf("[%lld tid=%d] %-22s ", now_ms(), gettid(), tag), printf(__VA_ARGS__), printf("\n")

extern int open(const char *, int, ...);
extern long read(int, void *, unsigned long);
/* thread census via /proc/self/status "Threads:" line */
static int count_threads(void) {
    char buf[1024];
    int fd = open("/proc/self/status", 0);
    if (fd < 0) return -1;
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    for (long i = 0; i < n - 8; i++) {
        if (buf[i]=='T' && !__builtin_memcmp(buf+i, "Threads:", 8)) {
            int v = 0; const char *p = buf + i + 8;
            while (*p == ' ' || *p == '\t') p++;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            return v;
        }
    }
    return -1;
}

/* libcamera_metadata */
extern size_t get_camera_metadata_entry_count(const void *metadata);
extern int get_camera_metadata_entry(const void *metadata, size_t index, void *entry);
extern int find_camera_metadata_entry(const void *metadata, uint32_t tag, void *entry);
extern int add_camera_metadata_entry(void *metadata, uint32_t tag, const void *data, size_t data_count);
extern int update_camera_metadata_entry(void *metadata, size_t index, const void *data, size_t data_count);

#define TAG_CAPTURE_PHYS_ID 0x805b005f  /* samsung.android.control.capturePhysicalId (INT32) */
#define TAG_RUNNING_PHYS_ID 0x805b0046  /* samsung.android.control.runningPhysicalId (BYTE[]) */
#define TAG_ZOOM_RATIO      0x805f0010  /* samsung.android.scaler.zoomRatio (FLOAT) */

typedef struct hw_module_methods_t {
    int (*open)(const void *module, const char *id, void **device);
} hw_module_methods_t;

#ifdef __aarch64__
typedef struct hw_module_t {
    uint32_t tag;                    /* 0x00 */
    uint16_t module_api_version;     /* 0x04 */
    uint16_t hal_api_version;        /* 0x06 */
    const char *id;                  /* 0x08 */
    const char *name;                /* 0x10 */
    const char *author;              /* 0x18 */
    const hw_module_methods_t *methods; /* 0x20 */
    void *dso;                       /* 0x28 */
    uint64_t reserved[26];           /* 0x30..0xF7 (empirical: fn ptrs at 0xF8) */
} hw_module_t;                       /* total 0xF8 */

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    const hw_module_t *module;      /* 0x08 */
    uint32_t reserved[12];
    int (*close)(void *device);     /* 0x70 */
} hw_device_t;
#else
typedef struct hw_module_t {
    uint32_t tag;                    /* 0x00 */
    uint16_t module_api_version;     /* 0x04 */
    uint16_t hal_api_version;        /* 0x06 */
    const char *id;                  /* 0x08 */
    const char *name;                /* 0x0c */
    const char *author;              /* 0x10 */
    const hw_module_methods_t *methods; /* 0x14 */
    void *dso;                       /* 0x18 */
    uint32_t reserved[25];           /* 0x1c..0x80 */
} hw_module_t;                       /* total 0x80 */

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    const hw_module_t *module;
    uint32_t reserved[12];
    int (*close)(void *device);
} hw_device_t;
#endif

/* gralloc1 minimal ABI (AOSP gralloc1.h, 32-bit) */
typedef uint32_t gralloc1_buffer_descriptor_t;  /* void* on 32-bit */
typedef struct { int32_t left, top, width, height; } gralloc1_rect_t;
typedef struct gralloc1_device {
    hw_device_t common;
    void (*getCapabilities)(struct gralloc1_device *, uint32_t *, int32_t *);
    void *(*getFunction)(struct gralloc1_device *, int32_t descriptor);
    void (*dump)(struct gralloc1_device *, uint32_t *, const void *);
    void *reserved[7];
} gralloc1_device_t;
#define G1_CREATE_DESCRIPTOR 2
#define G1_DESTROY_DESCRIPTOR 3
#define G1_SET_CONSUMER_USAGE 11
#define G1_SET_DIMENSIONS 12
#define G1_SET_FORMAT 13
#define G1_SET_PRODUCER_USAGE 14
#define G1_ALLOCATE 15
#define G1_LOCK 18
#define G1_UNLOCK 20
typedef int32_t (*g1_create_desc_fn)(gralloc1_device_t *, gralloc1_buffer_descriptor_t *);
typedef int32_t (*g1_set_dims_fn)(gralloc1_device_t *, gralloc1_buffer_descriptor_t, uint32_t, uint32_t);
typedef int32_t (*g1_set_fmt_fn)(gralloc1_device_t *, gralloc1_buffer_descriptor_t, int32_t);
typedef int32_t (*g1_set_usage_fn)(gralloc1_device_t *, gralloc1_buffer_descriptor_t, uint64_t);
typedef int32_t (*g1_alloc_fn)(gralloc1_device_t *, uint32_t, const gralloc1_buffer_descriptor_t *, native_handle_t **);
typedef int32_t (*g1_lock_fn)(gralloc1_device_t *, native_handle_t *, uint64_t, uint64_t, const gralloc1_rect_t *, void **, int32_t);



typedef struct camera_info {
    int facing;                      /* 0=BACK 1=FRONT 2=EXT */
    int orientation;
    uint32_t device_version;
    const void *static_camera_characteristics;
    int resource_cost;
    char **conflicting_devices;
    size_t conflicting_devices_length;
} camera_info_t;

typedef struct camera_module {
    hw_module_t common;                                  /* 0x80 */
    int (*get_number_of_cameras)(void);                  /* 0x80 */
    int (*get_camera_info)(int camera_id, camera_info_t *); /* 0x84 */
    int (*set_callbacks)(void *callbacks);               /* 0x88 */
    void (*get_vendor_tag_ops)(void *ops);               /* 0x8c */
    int (*open_legacy)(const void *, const char *, int, void **); /* 0x90 */
    int (*set_torch_mode)(const char *, int);            /* 0x94 */
    int (*init)(void);                                   /* 0x98 */
} camera_module_t;

/* camera3 callback ops: we implement these */
typedef struct camera3_callback_ops camera3_callback_ops_t;
struct camera3_callback_ops {
    void (*process_capture_result)(const camera3_callback_ops_t *, const void *result);
    void (*notify)(const camera3_callback_ops_t *, const void *msg);
    void *reserved[6]; /* request_buffer etc. */
};

/* camera3_device ops (function table after hw_device_t) */
typedef struct camera3_device {
    hw_device_t common;
    void *ops;   /* camera3_device_ops_t* */
    void *priv;
} camera3_device_t;
/* offsets differ 32/64 but pointer-relative access works via struct def */

typedef struct camera3_device_ops {
    int (*initialize)(const camera3_device_t *, const camera3_callback_ops_t *);  /* +0 */
    int (*configure_streams)(const camera3_device_t *, void *config);             /* +4 */
    int (*register_stream_buffers)(const camera3_device_t *, const void *);       /* +8 */
    const void *(*construct_default_request_settings)(const camera3_device_t *, int); /* +12 */
    int (*process_capture_request)(const camera3_device_t *, void *request);      /* +16 */
    void (*get_metadata_vendor_tag_ops)(const camera3_device_t *, void *);        /* +20 */
    void (*dump)(const camera3_device_t *, int fd);                               /* +24 */
    int (*flush)(const camera3_device_t *);                                       /* +28 */
    void *reserved[8];
} camera3_device_ops_t;

/* camera_metadata_entry (32-bit ABI): pointer FIRST, then value union */
typedef struct {
    uint32_t index;
    uint32_t tag;
    uint8_t  type;
    uint8_t  pad[3];
    uint32_t count;
    void    *data_ptr;   /* @16: array data (count > 1) */
    union { uint8_t u8; int32_t i32; float f; int64_t i64; double d; } data;  /* @20: scalar */
} ro_entry_t;

static void cb_result(const camera3_callback_ops_t *cb, const void *result) {
    /* camera3_capture_result_t: frame_number, result, num_output_buffers, output_buffers*, ... */
    const uint32_t *r = (const uint32_t *)result;
    uint32_t frame = r[0];
    uint32_t nob = r[2];
    const camera3_stream_buffer_t *obs = *(const camera3_stream_buffer_t **)(r + 3);
    n_results++;
    if (nob > 0) n_buffers_back++;
    /* one-time raw dump to verify the capture_result layout in this path */
    if (g_vid_dump && n_results <= 3) {
        LLOG("RESULT_RAW", "r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x",
             r[0], r[1], r[2], r[3], r[4], r[5]);
    }
    {
        const void *meta = (const void *)r[1];
        if (meta) {
            ro_entry_t e;
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_RUNNING_PHYS_ID, &e) == 0 && e.data_ptr)
                LLOG("RUNNING_PHYS_ID", "'%s'", (const char *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_ZOOM_RATIO, &e) == 0 && e.data_ptr)
                LLOG("RESULT_ZOOM", "%.2f", *(const float *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_LENS_APERTURE, &e) == 0 && e.data_ptr)
                LLOG("RESULT_APERTURE", "%.2f", *(const float *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, 0x800b0009 /*sensor current_mode*/, &e) == 0 && e.data_ptr)
                LLOG("RESULT_SENSOR_MODE", "%d", *(const int32_t *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, 0x80580009 /*PREV_LUX_INDEX*/, &e) == 0 && e.data_ptr)
                LLOG("RESULT_LUXIDX", "%.2f", *(const float *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, 0x80580002 /*SS_AEC_APEX*/, &e) == 0 && e.data_ptr)
                LLOG("RESULT_APEX", "%.2f", *(const float *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, 0x80580003 /*SS_AEC_EXPOSURE*/, &e) == 0 && e.data_ptr)
                LLOG("RESULT_SS_EXPOSURE", "%.2f", *(const float *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, 0x000e0000 /*sensor.exposureTime*/, &e) == 0 && e.data_ptr)
                LLOG("RESULT_EXPTIME", "%lld us", *(const long long *)e.data_ptr / 1000);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, 0x000e0001 /*sensor.frameDuration*/, &e) == 0 && e.data_ptr)
                LLOG("RESULT_FRAMEDUR", "%lld us", *(const long long *)e.data_ptr / 1000);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, 0x00010005 /*aeTargetFpsRange*/, &e) == 0 && e.data_ptr) {
                const int *fr = (const int *)e.data_ptr;
                LLOG("RESULT_FPSRANGE", "[%d %d] count=%u", fr[0], fr[1], e.count);
            }
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_OIS_MODE, &e) == 0 && e.data_ptr)
                LLOG("RESULT_OIS", "%d", *(const unsigned char *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_SS_OIS_OPMODE, &e) == 0 && e.data_ptr)
                LLOG("RESULT_OIS_VENDOR", "%d", *(const int32_t *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_SS_AEC_LIGHTCOND, &e) == 0 && e.data_ptr)
                LLOG("RESULT_LIGHTCOND", "%d", *(const int32_t *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_SS_AEC_APERTURECTL, &e) == 0 && e.data_ptr)
                LLOG("RESULT_APERCTL", "%d", *(const unsigned char *)e.data_ptr);
            memset(&e, 0, sizeof(e));
            if (find_camera_metadata_entry(meta, TAG_SS_AEC_APERTURETEST, &e) == 0 && e.data_ptr)
                LLOG("RESULT_APERTEST", "%d", *(const int32_t *)e.data_ptr);
        }
    }
    LLOG("CAPTURE_RESULT", "frame=%u outputs=%u (results=%d buffers_back=%d)",
         frame, nob, n_results, n_buffers_back);
    /* precise video-frame arrival timing: log every video-stream buffer */
    if (g_vid_dump && nob > 0 && obs) {
        for (uint32_t k = 0; k < nob; k++) {
            if (obs[k].stream == g_vid_stream) {
                static long long vprev = 0;
                struct { long sec; long nsec; } vts;   /* 32-bit bionic timespec */
                clock_gettime(1 /*CLOCK_MONOTONIC*/, &vts);
                long long vnow = vts.sec * 1000000LL + vts.nsec / 1000;
                LLOG("VID_FRAME", "frame=%u dt=%lld us", frame, vprev ? vnow - vprev : 0);
                vprev = vnow;
            }
        }
    }
    if (g_vid_dump && nob > 0 && obs && ((++g_vid_seen % g_vid_step) == 0)) {
        for (uint32_t k = 0; k < nob; k++) {
            if (!obs[k].buffer) continue;
            int second = 0;
            if (obs[k].stream == g_vid_stream) second = 0;
            else if (g_vid_stream2 && obs[k].stream == g_vid_stream2) second = 1;
            else continue;
            native_handle_t *h = *(obs[k].buffer);
            if (!h) continue;
            int fd = h->data[0];
            uint32_t dw, dh;
            if (second) { dw = g_vid_width2; dh = g_vid_height2; }
            else { dw = g_vid_width ? g_vid_width : g_st1.width;
                   dh = g_vid_height ? g_vid_height : g_st1.height; }
            uint32_t sz = dw * dh * 2;
            void *p = mmap(0, sz, 3, 1, fd, 0);
            if ((long)p == -1) continue;
            char sp[48]; const char *pre = second ? "/tmp/rawB" : "/tmp/vid";
            int q2 = 0; while (*pre) sp[q2++] = *pre++;
            int n = ++g_vid_seq;
            if (n >= 100) { sp[q2++] = '0' + n/100; n %= 100; }
            if (n >= 10 || g_vid_seq >= 10) { sp[q2++] = '0' + n/10; n %= 10; }
            sp[q2++] = '0' + n;
            const char *suf = ".raw"; while (*suf) sp[q2++] = *suf++;
            sp[q2] = 0;
            extern long write(int, const void *, unsigned long);
            int out = open(sp, 0x241, 0666);
            if (out < 0) {  /* /tmp missing (Android) — retry /data/local/tmp */
                q2 = 0; pre = second ? "/data/local/tmp/rawB" : "/data/local/tmp/vid";
                while (*pre) sp[q2++] = *pre++;
                n = ++g_vid_seq - 1;
                if (n >= 100) { sp[q2++] = '0' + n/100; n %= 100; }
                if (n >= 10 || g_vid_seq >= 10) { sp[q2++] = '0' + n/10; n %= 10; }
                sp[q2++] = '0' + n;
                suf = ".raw"; while (*suf) sp[q2++] = *suf++;
                sp[q2] = 0;
                out = open(sp, 0x241, 0666);
            }
            if (out >= 0) { write(out, p, sz); close(out); }
            munmap(p, sz);
            g_dump_every = 0;
        }
    }
    got_frame++;
}
static void cb_notify(const camera3_callback_ops_t *cb, const void *msg) {
    n_notify++;
    LLOG("NOTIFY", "msg=%p", msg);
}

static camera3_callback_ops_t callbacks = { cb_result, cb_notify, {0,0,0,0,0,0} };

__attribute__((used)) static void *zero_structors[1] = { 0 };
static const struct {
    void *preinit, *init, *fini, *term;
} structors_ __attribute__((used)) = { zero_structors, zero_structors, zero_structors, zero_structors };

/* ---- proper device shutdown: flush then close, exactly once each ---- */
static int do_flush_close(camera3_device_t *c3, camera3_device_ops_t *ops, hw_device_t *dev) {
    int rc = 0;
    if (ops->flush && !flush_called) {
        flush_called = 1;
        LLOG("FLUSH_BEGIN", "outstanding: submitted=%d results=%d buffers_back=%d",
             n_submitted, n_results, n_buffers_back);
        rc = ops->flush(c3);
        LLOG("FLUSH_END", "rc=%d submitted=%d results=%d buffers_back=%d notify=%d",
             rc, n_submitted, n_results, n_buffers_back, n_notify);
    }
    if (dev->close && !close_called) {
        close_called = 1;
        LLOG("CAMERA_CLOSE_BEGIN", "threads=%d", count_threads());
        rc = dev->close((void *)dev);
        LLOG("CAMERA_CLOSE_END", "rc=%d submitted=%d results=%d buffers_back=%d threads=%d",
             rc, n_submitted, n_results, n_buffers_back, count_threads());
        /* CamX worker threads are immortal singletons: they never exit after close
         * (16 alive 5s post-close; camx.log: "job not Unregistered"). flush()
         * already guarantees no outstanding requests, so only a short settle
         * window is needed to let any in-flight callback land. */
        extern void usleep(unsigned long);
        int t0 = count_threads();
        usleep(300000);
        LLOG("THREADS_SETTLED", "at_close=%d after_300ms=%d (immortal singletons)",
             t0, count_threads());
    }
    return rc;
}

/* one full open..close session; nreq = number of capture requests (0 = no configure) */
static int run_session2(camera_module_t *cam, int camid, int nreq, int dump,
                        uint32_t cw, uint32_t ch, const char *outpath,
                        const char *physid) {
    char idstr[8]; int il = 0, iv = camid;
    if (iv == 0) idstr[il++] = '0';
    else { char tm[8]; int k = 0; while (iv) { tm[k++] = '0' + iv % 10; iv /= 10; } while (k) idstr[il++] = tm[--k]; }
    idstr[il] = 0;
    hw_device_t *dev = 0;
    LLOG("CAMERA_OPEN", "camid=%d", camid);
    int rc = cam->common.methods->open(cam, idstr, (void **)&dev);
    LLOG("CAMERA_OPENED", "rc=%d dev=%p ver=%u (0x%x)", rc, dev,
         dev ? dev->version : 0, dev ? dev->version : 0);
    if (rc != 0 || !dev) return 1;

    camera3_device_t *c3 = (camera3_device_t *)dev;
    camera3_device_ops_t *ops = (camera3_device_ops_t *)c3->ops;
    LLOG("CAMERA_INITIALIZE", "ops=%p initialize=%p flush=%p",
         ops, ops->initialize, (void*)ops->flush);
    rc = ops->initialize(c3, &callbacks);
    LLOG("CAMERA_INITIALIZED", "rc=%d", rc);

    /* Samsung client identity (rawParam) via ops extension slot, when the
     * test asked for 60 fps — makes the CHI treat us as the Samsung app */
    if (g_fps_range > 0) {
        typedef int (*set_params_fn)(const camera3_device_t *, const char *);
        set_params_fn sp = (set_params_fn)ops->reserved[2];
        if (sp) {
            const char *raw = "first-entrance=false;samsungcamera=true;factorytest=false;"
                              "shootingmode=3;recording-fps=60;sw-vdis=false;"
                              "video-beautyface=false;vtmode=0;operation_mode=none;"
                              "ssm_shot_mode=0;recording_dr_mode=sdr;sw-super_vdis=false;stream_type=0;";
            int sprc = sp(c3, raw);
            LLOG("SETPARAMS", "rc=%d rawParam sent (fps60 session)", sprc);
        }
    }

    if (nreq <= 0) {
        /* Test A: open/initialize/close only */
        do_flush_close(c3, ops, dev);
        return 0;
    }

    extern void usleep(unsigned long);

    /* 1. default preview request settings */
    const void *reqmeta = ops->construct_default_request_settings(c3, 1 /*PREVIEW*/);
    LLOG("DEFAULT_SETTINGS", "%p", reqmeta);
    if (g_capture_phys >= 0 && reqmeta) {
        int v = g_capture_phys;
        int ar = add_camera_metadata_entry((void *)reqmeta, TAG_CAPTURE_PHYS_ID, &v, 1);
        LLOG("SET_CAPTURE_PHYS", "id=%d rc=%d", v, ar);
    }
    if (g_zoom_x100 >= 0 && reqmeta) {
        float zf = g_zoom_x100 / 100.0f;
        int ar = add_camera_metadata_entry((void *)reqmeta, TAG_ZOOM_RATIO, &zf, 1);
        LLOG("SET_ZOOM", "%.2f rc=%d", zf, ar);
        ar = add_camera_metadata_entry((void *)reqmeta, 0x0001002f /*ANDROID_CONTROL_ZOOM_RATIO*/, &zf, 1);
        LLOG("SET_STD_ZOOM", "%.2f rc=%d", zf, ar);
        ar = add_camera_metadata_entry((void *)reqmeta, 0x805b0013 /*zoomJumpTargetRatio*/, &zf, 1);
        LLOG("SET_ZOOM_JUMP", "%.2f rc=%d", zf, ar);
    }
    if ((g_fps_range > 0 || g_af_mode >= 0) && !g_req_clone && reqmeta) {
        /* clone the default request, dropping the tags we override (add fails on dup) */
        size_t cnt = get_camera_metadata_entry_count(reqmeta);
        void *dst = allocate_camera_metadata(cnt + 32, 262144);
        size_t copied = 0;
        if (dst) {
            for (size_t i = 0; i < cnt; i++) {
                ro_entry_t e;
                memset(&e, 0, sizeof(e));
                if (get_camera_metadata_entry(reqmeta, i, &e) != 0) continue;
                if (e.tag == 0x00010005 || e.tag == 0x000e0001 || e.tag == 0x0001000d) continue;
                if (g_af_mode >= 0 && e.tag == 0x00010007) continue;
                const void *dptr = e.data_ptr ? e.data_ptr : (const void *)&e.data;
                if (add_camera_metadata_entry(dst, e.tag, dptr, e.count) == 0) copied++;
            }
            g_req_clone = dst;
            LLOG("REQ_CLONE", "src=%zu copied=%zu", cnt, copied);
        }
    }
    if (g_req_clone) reqmeta = g_req_clone;
    if (g_af_mode >= 0 && reqmeta) {
        unsigned char am = (unsigned char)g_af_mode;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x00010007 /*control.afMode*/, &am, 1);
        LLOG("SET_AFMODE", "%d rc=%d", g_af_mode, ar);
    }
    if (g_crop_x100 >= 0 && reqmeta) {
        /* centered crop = active/zoom */
        int cw = 4032 * 100 / g_crop_x100, chh = 3024 * 100 / g_crop_x100;
        int rect[4] = { (4032 - cw) / 2, (3024 - chh) / 2, cw, chh };
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x000d0000 /*SCALER_CROP_REGION*/, rect, 4);
        LLOG("SET_CROP", "%d,%d %dx%d rc=%d", rect[0], rect[1], cw, chh, ar);
    }
    if (g_recording_max_fps > 0 && reqmeta) {
        int mv = g_recording_max_fps;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x805b0050 /*recordingMaxFps*/, &mv, 1);
        LLOG("SET_REC_FPS", "%d rc=%d", mv, ar);
        int tv = mv;
        ar = add_camera_metadata_entry((void *)reqmeta, 0x800b000a /*targetFPS*/, &tv, 1);
        LLOG("SET_TARGET_FPS", "%d rc=%d", tv, ar);
    }
    if (g_capture_mode_hint >= 0 && reqmeta) {
        int v = g_capture_mode_hint;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x805b0034 /*captureMode*/, &v, 1);
        LLOG("SET_CAPMODE", "%d rc=%d", v, ar);
    }
    if (g_recording_hint >= 0 && reqmeta) {
        unsigned char hv = (unsigned char)g_recording_hint;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x805b0038 /*recordingHint*/, &hv, 1);
        LLOG("SET_REC_HINT", "%d rc=%d", g_recording_hint, ar);
    }
    if (g_sensor_mode >= 0 && reqmeta) {
        int smv = g_sensor_mode;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x800b0002 /*sensor_mode_index*/, &smv, 1);
        LLOG("SET_SENSOR_MODE", "%d rc=%d", smv, ar);
    }
    if (g_fps_range > 0 && reqmeta) {
        int fr[2] = { g_fps_range, g_fps_range };
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x00010005 /*AE_TARGET_FPS_RANGE*/, fr, 2);
        LLOG("SET_FPS", "%d-%d rc=%d", fr[0], fr[1], ar);
        long long fd = 1000000000LL / g_fps_range;
        ar = add_camera_metadata_entry((void *)reqmeta, 0x000e0001 /*sensor.frameDuration*/, &fd, 1);
        LLOG("SET_FRAMEDUR", "%lld rc=%d", fd, ar);
        unsigned char ci = 3; /* VIDEO_RECORD */
        ar = add_camera_metadata_entry((void *)reqmeta, 0x0001000d /*captureIntent*/, &ci, 1);
        LLOG("SET_INTENT", "VIDEO_RECORD rc=%d", ar);
        int smv3 = 3;
        ar = add_camera_metadata_entry((void *)reqmeta, 0x805b000d /*shootingMode=VIDEO*/, &smv3, 1);
        LLOG("SET_SHOOTINGMODE", "3 rc=%d", ar);
    }
    if (g_lightcond >= 0 && reqmeta) {
        int lv = g_lightcond;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x80580004 /*SS_AEC_LIGHTCONDITION*/, &lv, 1);
        LLOG("SET_LIGHTCOND", "%d rc=%d", lv, ar);
    }
    if (g_apex_x10 >= 0 && reqmeta) {
        float ax = g_apex_x10 / 10.0f;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x80580002 /*SS_AEC_APEX*/, &ax, 1);
        LLOG("SET_APEX", "%.1f rc=%d", ax, ar);
        float lx = ax; /* PREV_LUX_INDEX same scale guess */
        ar = add_camera_metadata_entry((void *)reqmeta, 0x80580009 /*PREV_LUX_INDEX*/, &lx, 1);
        LLOG("SET_LUXIDX", "%.1f rc=%d", lx, ar);
    }
    if (g_ois_mode >= 0 && reqmeta) {
        unsigned char on = (unsigned char)g_ois_mode;
        int ar = add_camera_metadata_entry((void *)reqmeta, TAG_OIS_MODE, &on, 1);
        LLOG("SET_OIS", "mode=%d rc=%d", g_ois_mode, ar);
        int vm = g_ois_mode;
        ar = add_camera_metadata_entry((void *)reqmeta, TAG_SS_OIS_OPMODE, &vm, 1);
        LLOG("SET_OIS_VENDOR", "opmode=%d rc=%d", vm, ar);
    }
    if (g_aperture >= 0 && reqmeta) {
        unsigned char av = (unsigned char)g_aperture;
        int ar = add_camera_metadata_entry((void *)reqmeta, TAG_SS_AEC_APERTURECTL, &av, 1);
        LLOG("SET_APERTURE_CTL", "%d rc=%d", g_aperture, ar);
        int at = g_aperture;
        ar = add_camera_metadata_entry((void *)reqmeta, TAG_SS_AEC_APERTURETEST, &at, 1);
        LLOG("SET_APERTURE_TEST", "%d rc=%d", at, ar);
    }
    if (g_capture_mode >= 0 && reqmeta) {
        int cm = g_capture_mode;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x805b0034 /*captureMode*/, &cm, 1);
        LLOG("SET_CAPTURE_MODE", "%d rc=%d", cm, ar);
    }
    if (g_samsung_cam >= 0 && reqmeta) {
        unsigned char b = (unsigned char)g_samsung_cam;
        int ar = add_camera_metadata_entry((void *)reqmeta, 0x805b0037 /*samsungCamera*/, &b, 1);
        LLOG("SET_SAMSCAM", "%d rc=%d", g_samsung_cam, ar);
    }

    /* 2. preview + full-res YUV capture streams */
    camera3_stream_t *st = &g_st0;
    memset(st, 0, sizeof(*st));
    st->stream_type = 0;          /* OUTPUT preview */
    st->width = g_prev_w; st->height = g_prev_h;
    st->format = g_fmt0;
    st->usage = g_use0;
    st->max_buffers = 4;
    camera3_stream_t *st2 = &g_st1;
    memset(st2, 0, sizeof(*st2));
    st2->stream_type = 0;         /* OUTPUT capture */
    st2->width = cw; st2->height = ch;
    st2->format = g_fmt1;
    st2->usage = g_use1;
    st2->max_buffers = g_cap_buffers;
    st->physical_camera_id = g_preview_physid;  /* API 3.5 physical stream routing */
    st2->physical_camera_id = physid;   /* API 3.5: route stream to a physical camera */
    g_sarr[0] = st; g_sarr[1] = st2;
    int nst = 2;
    if (g_use_3rd) {
        memset(&g_st3, 0, sizeof(g_st3));
        g_st3.stream_type = 0;
        g_st3.width = g_prev_w; g_st3.height = g_prev_h;
        g_st3.format = 0x23; g_st3.usage = 0x20003;
        g_st3.max_buffers = 4;
        g_sarr[2] = &g_st3;
        nst = 3;
    }
    camera3_stream_t **sarr = g_sarr;
    camera3_stream_configuration_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_streams = nst;
    cfg.streams = sarr;
    cfg.operation_mode = 0;
    cfg.session_parameters = 0;
    if (g_zoom_x100 >= 0 || g_samsung_cam >= 0 || g_fps_range > 0) {
        cfg.session_parameters = build_session_params(g_zoom_x100);
        LLOG("SESSION_PARAMS", "%p", cfg.session_parameters);
    }
    cfg.input_width = 0; cfg.input_height = 0;
    if (g_poke_fps > 0) {
        unsigned long base = get_chi_base();
        if (base) {
#ifdef __aarch64__
            /* 64-bit CHI FPS global offset (to be located); poke 64-bit .bss */
            volatile unsigned int *p = (unsigned int *)(base + 0x28D860);
#else
            volatile unsigned int *p = (unsigned int *)(base + 0x2772EC);
#endif
            unsigned int old = *p;
            *p = (unsigned int)g_poke_fps;
            LLOG("POKE_FPS", "chi@0x%lx global 0x%lx: %u -> %u", base, base + 0x2772EC, old, g_poke_fps);
        } else {
            LLOG("POKE_FPS", "chi base NOT found");
        }
    }
    LLOG("CONFIGURE_STREAMS", "num=%d", cfg.num_streams);
    rc = ops->configure_streams(c3, &cfg);
    LLOG("CONFIGURED", "rc=%d (w=%u h=%u fmt=0x%x usage=0x%x maxbuf=%u)",
         rc, st->width, st->height, st->format, st->usage, st->max_buffers);
    if (rc != 0) { do_flush_close(c3, ops, dev); return 2; }

    /* 3. ion-allocate + hand-craft QTI private_handle_t (numFds=2, numInts=23, magic 'gmsm') */
    int ion = open("/dev/ion", 0);
    if (ion < 0) { printf("[shim] ion open failed\n"); do_flush_close(c3, ops, dev); return 3; }
    uint32_t bufsize = st2->width * st2->height * 2;
    native_handle_t *handles[16];
    for (int i = 0; i < g_cap_buffers && i < 16; i++) {
        struct ion_alloc_414 ia; memset(&ia, 0, sizeof(ia));
        ia.len = bufsize; ia.heap_mask = 0x2000000; ia.flags = 0;
        if (ioctl(ion, ION_IOC_ALLOC_414, &ia) != 0) {
            printf("[shim] ion alloc %d failed\n", i);
            do_flush_close(c3, ops, dev); return 3;
        }
        int bfd = (int)ia.fd;
        /* private_handle_t: header(3 ints) + fd, fd_meta + 23 ints */
        int *h = (int *)malloc(sizeof(int) * (3 + 2 + 23));
        memset(h, 0, sizeof(int) * 28);
        h[0] = 12;        /* version = sizeof(native_handle_t) */
        h[1] = 2;         /* numFds */
        h[2] = 23;        /* numInts */
        h[3] = bfd;       /* fd */
        h[4] = -1;        /* fd_meta: none */
        h[5] = 0x6d736d67;/* magic 'gmsm' (LE) */
        h[6] = 0x80000000;/* PRIV_FLAGS_USES_ION */
        h[7] = bufsize;   /* size */
        h[8] = 0;         /* offset */
        h[9] = 0;         /* offset_meta */
        h[10] = st2->format; /* format */
        h[11] = st2->width;  /* width */
        h[12] = st2->height; /* height */
        handles[i] = (native_handle_t *)h;
        frame_fd[i] = bfd; frame_size[i] = bufsize;
        printf("[shim] buf %d: fd=%d size=%u\n", i, bfd, bufsize);
    }

    /* 3a2. third-stream (previewCb YUV) buffers */
    if (g_use_3rd) {
        uint32_t sz3 = g_st3.width * g_st3.height * 2;
        for (int i = 0; i < 4; i++) {
            struct ion_alloc_414 ia; memset(&ia, 0, sizeof(ia));
            ia.len = sz3; ia.heap_mask = 0x2000000; ia.flags = 0;
            if (ioctl(ion, ION_IOC_ALLOC_414, &ia) != 0) { printf("[shim] ion 3rd alloc failed\n"); break; }
            int bfd = (int)ia.fd;
            int *h = (int *)malloc(sizeof(int) * 28);
            memset(h, 0, sizeof(int) * 28);
            h[0] = 12; h[1] = 2; h[2] = 23; h[3] = bfd; h[4] = -1;
            h[5] = 0x6d736d67; h[6] = 0x80000000; h[7] = sz3; h[8] = 0;
            h[9] = 0; h[10] = g_st3.format; h[11] = g_st3.width; h[12] = g_st3.height;
            g_st3_handles[i] = (native_handle_t *)h; g_st3_fd[i] = bfd;
        }
    }

    /* 3b. preview-stream buffers (real clients feed ALL streams) */
    uint32_t psize = st->width * st->height * 2;
    native_handle_t *pvhandles[2];
    int pvfd[2];
    for (int i = 0; i < 2; i++) {
        struct ion_alloc_414 ia; memset(&ia, 0, sizeof(ia));
        ia.len = psize; ia.heap_mask = 0x2000000; ia.flags = 0;
        if (ioctl(ion, ION_IOC_ALLOC_414, &ia) != 0) { printf("[shim] ion preview alloc failed\n"); break; }
        int bfd = (int)ia.fd;
        int *h = (int *)malloc(sizeof(int) * 28);
        memset(h, 0, sizeof(int) * 28);
        h[0] = 12; h[1] = 2; h[2] = 23; h[3] = bfd; h[4] = -1;
        h[5] = 0x6d736d67; h[6] = 0x80000000; h[7] = psize; h[8] = 0;
        h[9] = 0; h[10] = st->format; h[11] = st->width; h[12] = st->height;
        pvhandles[i] = (native_handle_t *)h; pvfd[i] = bfd;
    }

    /* 4. capture requests: rotate the buffers, submit preview+capture */
    for (int f = 0; f < nreq; f++) {
        camera3_stream_buffer_t obs[3];
        memset(obs, 0, sizeof(obs));
        obs[0].stream = st;
        native_handle_t **phslot = (native_handle_t **)malloc(sizeof(void *));
        *phslot = pvhandles[f & 1];
        obs[0].buffer = phslot;
        obs[0].status = 0; obs[0].acquire_fence = -1; obs[0].release_fence = -1;
        obs[1].stream = st2;
        native_handle_t **hslot = (native_handle_t **)malloc(sizeof(void *));
        *hslot = handles[f % g_cap_buffers];
        obs[1].buffer = hslot;
        obs[1].status = 0; obs[1].acquire_fence = -1; obs[1].release_fence = -1;
        if (g_use_3rd) {
            obs[2].stream = &g_st3;
            native_handle_t **s3slot = (native_handle_t **)malloc(sizeof(void *));
            *s3slot = g_st3_handles[f & 3];
            obs[2].buffer = s3slot;
            obs[2].status = 0; obs[2].acquire_fence = -1; obs[2].release_fence = -1;
        }
        camera3_capture_request_t req;
        memset(&req, 0, sizeof(req));
        req.frame_number = f + 1;
        req.settings = reqmeta;
        req.input_buffer = 0;
        req.output_buffers = obs;
        req.num_output_buffers = g_use_3rd ? 3 : 2;
        req.input_width = 0; req.input_height = 0;
        if (g_req_delay_ms > 0 && f > 0) usleep(g_req_delay_ms * 1000);
        LLOG("REQUEST_SUBMITTED", "frame=%d", f + 1);
        rc = ops->process_capture_request(c3, &req);
        if (rc == 0) n_submitted++;
        LLOG("REQUEST_RETURNED", "frame=%d rc=%d submitted=%d", f + 1, rc, n_submitted);
        if (g_dump_every > 0 && ((f + 1) % g_dump_every) == 0) {
            int bi = f % g_cap_buffers;
            void *p = mmap(0, frame_size[bi], 3 /*RW*/, 1 /*SHARED*/, frame_fd[bi], 0);
            extern long write(int, const void *, unsigned long);
            char sp[48]; const char *pre = g_dump_prefix;
            int k = 0; while (*pre) sp[k++] = *pre++;
            int n = (f + 1) / g_dump_every;
            if (n >= 10) { sp[k++] = '0' + n / 10; n %= 10; }
            sp[k++] = '0' + n;
            const char *suf = ".raw"; while (*suf) sp[k++] = *suf++;
            sp[k] = 0;
            int out = open(sp, 0x241, 0666);
            if (out >= 0 && (long)p != -1) { write(out, p, frame_size[f & 3]); close(out); }
            LLOG("SLOW_DUMP", "%s", sp);
        }
    }

    /* 5. wait until every submitted request returned its output buffer */
    for (int w = 0; w < 100 && n_buffers_back < n_submitted; w++) usleep(100000);
    LLOG("WAIT_DONE", "submitted=%d results=%d buffers_back=%d notify=%d",
         n_submitted, n_results, n_buffers_back, n_notify);

    /* 6. dump frame 0 */
    if (dump && got_frame > 0) {
        void *p = mmap(0, frame_size[0], 3 /*RW*/, 1 /*SHARED*/, frame_fd[0], 0);
        extern long write(int, const void *, unsigned long);
        int out = open(outpath, 0x241 /*O_WRONLY|O_CREAT|O_TRUNC*/, 0666);
        if (out >= 0 && (long)p != -1) {
            write(out, p, frame_size[0]);
            close(out);
            printf("[shim] dumped %u bytes to /tmp/frame0.raw\n", frame_size[0]);
        } else printf("[shim] dump failed: map=%p out=%d\n", p, out);
    }

    /* 7. orderly shutdown while buffers/handshakes are still alive */
    rc = do_flush_close(c3, ops, dev);

    /* 8. HAL has released everything: now free shim-owned resources */
    LLOG("SHIM_FREE", "freeing %d+2 handles + ion fds", g_cap_buffers);
    for (int i = 0; i < g_cap_buffers; i++) {
        free(handles[i]);
        if (frame_fd[i] > 0) close(frame_fd[i]);
    }
    for (int i = 0; i < 2; i++) {
        free(pvhandles[i]);
        if (pvfd[i] > 0) close(pvfd[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (g_st3_handles[i]) free(g_st3_handles[i]);
        if (g_st3_fd[i] > 0) close(g_st3_fd[i]);
        g_st3_handles[i] = 0; g_st3_fd[i] = -1;
    }
    close(ion);
    return rc;
}

/* set-or-update a metadata tag (default request already contains many tags) */
static int set_or_update_tag(void *meta, uint32_t tag, const void *data, size_t count) {
    ro_entry_t e;
    int rc;
    memset(&e, 0, sizeof(e));
    rc = find_camera_metadata_entry(meta, tag, &e);
    printf("[sut] tag=0x%x find rc=%d index=%zu count=%u\n", tag, rc, (size_t)e.index, e.count);
    if (rc == 0) {
        rc = update_camera_metadata_entry(meta, (size_t)e.index, data, count);
        printf("[sut] tag=0x%x update rc=%d\n", tag, rc);
        return rc;
    }
    rc = add_camera_metadata_entry(meta, tag, data, count);
    printf("[sut] tag=0x%x add rc=%d\n", tag, rc);
    return rc;
}

static void *build_session_params(int zoom_x100) {
    void *m = allocate_camera_metadata(32, 4096);
    if (!m) return 0;
    unsigned char samsung = 1;
    add_camera_metadata_entry(m, 0x805b0037 /*samsungCamera*/, &samsung, 1);
    if (zoom_x100 >= 0) {
        float zf = zoom_x100 / 100.0f;
        add_camera_metadata_entry(m, 0x805f0010 /*samsung zoomRatio*/, &zf, 1);
        add_camera_metadata_entry(m, 0x0001002f /*ANDROID_CONTROL_ZOOM_RATIO*/, &zf, 1);
        add_camera_metadata_entry(m, 0x805b0013 /*zoomJumpTargetRatio*/, &zf, 1);
    }
    if (g_fps_range > 0) {
        /* CHI picks pipelines/sensor modes at configure time — must see 60fps here */
        int fr[2] = { g_fps_range, g_fps_range };
        add_camera_metadata_entry(m, 0x00010005 /*AE_TARGET_FPS_RANGE*/, fr, 2);
        long long fd = 1000000000LL / g_fps_range;
        add_camera_metadata_entry(m, 0x000e0001 /*sensor.frameDuration*/, &fd, 1);
        unsigned char ci = 3; /* VIDEO_RECORD */
        add_camera_metadata_entry(m, 0x0001000d /*captureIntent*/, &ci, 1);
        int sm = 3;
        add_camera_metadata_entry(m, 0x805b000d /*shootingMode=VIDEO*/, &sm, 1);
        int mv = 60;
        add_camera_metadata_entry(m, 0x805b0050 /*recordingMaxFps*/, &mv, 1);
        add_camera_metadata_entry(m, 0x800b000a /*targetFPS*/, &mv, 1);
        unsigned char hv = 1;
        add_camera_metadata_entry(m, 0x805b0038 /*recordingHint*/, &hv, 1);
        int cm = 3;
        add_camera_metadata_entry(m, 0x805b0034 /*captureMode*/, &cm, 1);
        if (g_sensor_mode >= 0)
            add_camera_metadata_entry(m, 0x800b0002 /*sensor_mode_index*/, &g_sensor_mode, 1);
        LLOG("SESSPARAM_FPS", "%d sm=%d", g_fps_range, g_sensor_mode);
    }
    return m;
}

/* ---- 4K60 stock-replica session: the 5-stream config the app uses ---- */
/* ---- ToF depth capture: IMX516, single Y16 (0x20363159) output stream ----
 * The ToF camera is a DEPTH_OUTPUT camera; its frames are 16-bit depth (Y16).
 * Default camera id 4 (the enumerated IMX516 on the plain HAL). */
static int run_tof(camera_module_t *cam, int camid, int nreq) {
    static camera3_stream_t TS;
    /* args: "t [w h fmt [nframes]]"  (fmt may be hex w/ 0x; bit31 = data_space 0x1000) */
    int tw = (g_argc > 3) ? atoi_(g_argv[3]) : 640;
    int th = (g_argc > 4) ? atoi_(g_argv[4]) : 480;
    int tf = 0x20363159;
    if (g_argc > 5) { const char *fp = g_argv[5]; tf = 0;
        if (fp[0]=='0'&&fp[1]=='x') fp += 2;
        while (*fp) { int c=*fp; int d=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; if(d<0)break; tf=tf*16+d; fp++; } }
    if (g_argc > 6) { int n = atoi_(g_argv[6]); if (n > 0) nreq = n; }
    camera3_stream_t *sarr[1];
    char idstr[8]; int il = 0, iv = camid;
    if (iv == 0) idstr[il++] = '0';
    else { char tm[8]; int k = 0; while (iv) { tm[k++] = '0' + iv % 10; iv /= 10; } while (k) idstr[il++] = tm[--k]; }
    idstr[il] = 0;
    hw_device_t *dev = 0;
    int rc;

    LLOG("TOF_TRY", "cam%d %d frames", camid, nreq);
    rc = cam->common.methods->open(cam, idstr, (void **)&dev);
    LLOG("TOF_OPENED", "rc=%d", rc);
    if (rc != 0 || !dev) return 1;
    camera3_device_t *c3 = (camera3_device_t *)dev;
    camera3_device_ops_t *ops = (camera3_device_ops_t *)c3->ops;
    rc = ops->initialize(c3, &callbacks);
    LLOG("TOF_INIT", "rc=%d", rc);

    /* Samsung client identity — mark the session as a Samsung app */
    {
        typedef int (*set_params_fn)(const camera3_device_t *, const char *);
        set_params_fn sp = (set_params_fn)ops->reserved[2];
        if (sp) {
            const char *raw = "first-entrance=false;samsungcamera=true;factorytest=false;"
                              "shootingmode=1;recording-fps=0;sw-vdis=false;"
                              "video-beautyface=false;vtmode=0;operation_mode=none;"
                              "ssm_shot_mode=0;recording_dr_mode=sdr;sw-super_vdis=false;stream_type=0;";
            LLOG("TOF_SETPARAMS", "rc=%d", sp(c3, raw));
        }
    }

    /* single Y16 output stream: 640x480 depth */
    memset(&TS, 0, sizeof(TS));
    TS.stream_type = 0;                 /* OUTPUT */
    TS.width = tw; TS.height = th;
    TS.format = tf;             /* default HAL_PIXEL_FORMAT_Y16 */
    TS.usage = 0x3;                     /* SW read */
    TS.max_buffers = 8;
    /* SecCamTOF (chiTOF RE): the ToF session stream is format 0x21 with
       data_space 0x1000. Optional fmt flag: fmt with bit31 = set data_space 0x1000. */
    if (tf & 0x80000000) { TS.format = tf & 0x7fffffff; TS.data_space = 0x1000; }
    sarr[0] = &TS;

    camera3_stream_configuration_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_streams = 1; cfg.streams = sarr;
    cfg.operation_mode = 0;
    const void *reqmeta = ops->construct_default_request_settings(c3, 1 /*PREVIEW*/);
    LLOG("TOF_DEFAULTS", "%p", reqmeta);

    rc = ops->configure_streams(c3, &cfg);
    LLOG("TOF_CONFIGURED", "rc=%d", rc);
    if (rc != 0) { do_flush_close(c3, ops, dev); return 2; }

    /* SecCamTOFUsecase::Create MUTATES our 0x21+ds0x1000 stream in place:
       it becomes the point-cloud output container (BLOB convention: height=1,
       width=(capsW/2)*(capsH/8)*16+36 = 4915236 for the IMX516's 1280x3840 caps).
       CamX's TOFPointCloudOut1 pipeline then imports OUR buffers at
       640*480*16+48 = 4915248 bytes.  If our ion buffer is smaller, the
       dma_buf mmap() fails EINVAL and every request errors out (the sensor
       never streams -> the all-zero frames).  Size our buffers from the
       post-configure (mutated) stream dims, rounded up to pages. */
    LLOG("TOF_MUTATED", "post-config stream: %dx%d fmt=0x%x", TS.width, TS.height, TS.format);
    uint32_t want = (uint32_t)tw * th * 2;
    if (TS.height == 1 && TS.width > want) want = TS.width;      /* BLOB-mutated */
    want = (want + 48 + 4095) & ~4095u;                          /* +hdr, page align */
    uint32_t sz = want;

    /* buffers: 8 frames */
    static native_handle_t *H[8];
    static int F[8];
    LLOG("TOF_DIMS", "%dx%d fmt=0x%x bufsz=%u", tw, th, tf, sz);
    int ion = open("/dev/ion", 0);
    for (int j = 0; j < 8; j++) {
        struct ion_alloc_414 ia; memset(&ia, 0, sizeof(ia));
        ia.len = sz; ia.heap_mask = 0x2000000; ia.flags = 0;
        if (ioctl(ion, ION_IOC_ALLOC_414, &ia) != 0) { printf("[shim] tof alloc %d failed\n", j); return 3; }
        int bfd = (int)ia.fd;
        int *h = (int *)malloc(sizeof(int) * 28);
        memset(h, 0, sizeof(int) * 28);
        h[0] = 12; h[1] = 2; h[2] = 23; h[3] = bfd; h[4] = -1;
        h[5] = 0x6d736d67; h[6] = 0x80000000; h[7] = sz; h[8] = 0;
        h[9] = 0; h[10] = TS.format; h[11] = TS.width; h[12] = TS.height;
        H[j] = (native_handle_t *)h; F[j] = bfd;
    }

    /* dump every frame via the shared vid-dump path (dims overridden) */
    g_vid_dump = 1; g_vid_seq = 0; g_vid_seen = 0; g_vid_step = 1;
    g_vid_stream = &TS; g_vid_width = sz / 2; g_vid_height = 1;  /* dump the whole (blob) buffer */

    for (int f = 0; f < nreq; f++) {
        /* Fire the ToF VCSEL laser once the sensor session is acquired (g_s_ctrl_tof
           is set on CAM_ACQUIRE_DEV, which happens at the first request). The laser
           is NOT auto-enabled by the stock driver; without it the frames are dark.
           Retry until the sysfs write sticks (the store fails with EPERM while the
           sensor isn't acquired yet). */
        if (!g_tof_laser_on) {
            int ld = open("/sys/devices/virtual/camera/rear/rear_tof_ld_onoff", 1 /*O_WRONLY*/);
            if (ld >= 0) {
                extern long write(int, const void *, unsigned long);
                if (write(ld, "1", 1) == 1) { g_tof_laser_on = 1; LLOG("TOF_LASER", "VCSEL on (frame %d)", f + 1); }
                close(ld);
            }
        }
        camera3_stream_buffer_t ob;
        native_handle_t **slot;
        memset(&ob, 0, sizeof(ob));
        ob.stream = &TS;
        slot = (native_handle_t **)malloc(sizeof(void *));
        *slot = H[f % 8];
        ob.buffer = slot;
        ob.status = 0; ob.acquire_fence = -1; ob.release_fence = -1;
        camera3_capture_request_t req;
        memset(&req, 0, sizeof(req));
        req.frame_number = f + 1;
        req.settings = reqmeta;
        req.input_buffer = 0;
        req.output_buffers = &ob;
        req.num_output_buffers = 1;
        req.input_width = 0; req.input_height = 0;
        rc = ops->process_capture_request(c3, &req);
        if (rc != 0) { LLOG("TOF_REQFAIL", "frame=%d rc=%d", f + 1, rc); break; }
        n_submitted++;
        usleep(200000);   /* ToF is slow (5 fps at 640x480) — pace the requests */
    }
    for (int w = 0; w < 300 && n_buffers_back < n_submitted; w++) usleep(20000);
    LLOG("TOF_WAITDONE", "submitted=%d results=%d buffers_back=%d", n_submitted, n_results, n_buffers_back);
    dump_all_dmabufs("tof");   /* recover the pipeline's internal raw-phase buffers */

    do_flush_close(c3, ops, dev);
    for (int j = 0; j < 8; j++) { free(H[j]); if (F[j] > 0) close(F[j]); }
    close(ion);
    g_vid_dump = 0; g_vid_width = 0; g_vid_height = 0; g_vid_stream = &g_st1;
    LLOG("TOF_END", "rc=%d", rc);
    return rc;
}

/* 2-stream ToF: stream0 = 0x21 ds0x1000 (raw ToF trigger -> SecCamTOF, becomes the
   pointcloud output), stream1 = a second framework stream we want the usecase to
   fill (e.g. 0x20 RAW16 1280x3840 to tap the raw phase, or 0x20363159 depth16).
   args: T w1 h1 fmt2 w2 h2 [nframes]   (stream1 always 0x21+ds0x1000) */
static int run_tof2(camera_module_t *cam, int camid, int nreq) {
    static camera3_stream_t S0, S1;
    int w1 = (g_argc > 3) ? atoi_(g_argv[3]) : 640;
    int h1 = (g_argc > 4) ? atoi_(g_argv[4]) : 480;
    int fmt2 = (g_argc > 5) ? htoi_(g_argv[5]) : 0x20;
    int w2 = (g_argc > 6) ? atoi_(g_argv[6]) : 1280;
    int h2 = (g_argc > 7) ? atoi_(g_argv[7]) : 3840;
    if (g_argc > 8) { int n = atoi_(g_argv[8]); if (n > 0) nreq = n; }
    camera3_stream_t *sarr[2];
    char idstr[8]; int il = 0, iv = camid;
    if (iv == 0) idstr[il++] = '0';
    else { char tm[8]; int k = 0; while (iv) { tm[k++] = '0' + iv % 10; iv /= 10; } while (k) idstr[il++] = tm[--k]; }
    idstr[il] = 0;
    hw_device_t *dev = 0;
    int rc;

    LLOG("TOF2_TRY", "cam%d raw %dx%d + fmt2=0x%x %dx%d n=%d", camid, w1, h1, fmt2, w2, h2, nreq);
    rc = cam->common.methods->open(cam, idstr, (void **)&dev);
    if (rc != 0 || !dev) { LLOG("TOF2_OPEN", "rc=%d", rc); return 1; }
    camera3_device_t *c3 = (camera3_device_t *)dev;
    camera3_device_ops_t *ops = (camera3_device_ops_t *)c3->ops;
    rc = ops->initialize(c3, &callbacks);
    LLOG("TOF2_INIT", "rc=%d", rc);

    {   /* Samsung client identity */
        typedef int (*set_params_fn)(const camera3_device_t *, const char *);
        set_params_fn sp = (set_params_fn)ops->reserved[2];
        if (sp) sp(c3, "first-entrance=false;samsungcamera=true;factorytest=false;shootingmode=1;recording-fps=0;");
    }

    memset(&S0, 0, sizeof(S0));
    S0.stream_type = 0; S0.width = w1; S0.height = h1;
    S0.format = 0x21; S0.usage = 0x3; S0.max_buffers = 8; S0.data_space = 0x1000;
    memset(&S1, 0, sizeof(S1));
    S1.stream_type = 0; S1.width = w2; S1.height = h2;
    S1.format = fmt2; S1.usage = 0x3; S1.max_buffers = 8; S1.data_space = 0x1000;
    sarr[0] = &S0; sarr[1] = &S1;

    camera3_stream_configuration_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_streams = 2; cfg.streams = sarr; cfg.operation_mode = 0;
    const void *reqmeta = ops->construct_default_request_settings(c3, 1);
    rc = ops->configure_streams(c3, &cfg);
    LLOG("TOF2_CONFIGURED", "rc=%d  S0=%dx%d(0x%x,ds%x)  S1=%dx%d(0x%x,ds%x)", rc,
         S0.width, S0.height, S0.format, S0.data_space, S1.width, S1.height, S1.format, S1.data_space);
    if (rc != 0) { do_flush_close(c3, ops, dev); return 2; }

    /* per-stream buffer sizing (post-mutation for S0) */
    uint32_t sz0 = (uint32_t)w1 * h1 * 2;
    if (S0.height == 1 && S0.width > sz0) sz0 = S0.width;
    sz0 = (sz0 + 48 + 4095) & ~4095u;
    uint32_t sz1 = (uint32_t)w2 * h2 * 2;
    if (S1.height == 1 && S1.width > sz1) sz1 = S1.width;
    sz1 = (sz1 + 48 + 4095) & ~4095u;
    LLOG("TOF2_BUFSZ", "sz0=%u sz1=%u", sz0, sz1);

    static native_handle_t *H0[8], *H1[8];
    static int F0[8], F1[8];
    int ion = open("/dev/ion", 0);
    for (int j = 0; j < 8; j++) {
        for (int s = 0; s < 2; s++) {
            uint32_t ssz = s ? sz1 : sz0;
            struct ion_alloc_414 ia; memset(&ia, 0, sizeof(ia));
            ia.len = ssz; ia.heap_mask = 0x2000000; ia.flags = 0;
            if (ioctl(ion, ION_IOC_ALLOC_414, &ia) != 0) { LLOG("TOF2_ALLOC", "fail s%d j%d", s, j); return 3; }
            int bfd = (int)ia.fd;
            int *h = (int *)malloc(sizeof(int) * 28);
            memset(h, 0, sizeof(int) * 28);
            camera3_stream_t *sp = s ? &S1 : &S0;
            h[0] = 12; h[1] = 2; h[2] = 23; h[3] = bfd; h[4] = -1;
            h[5] = 0x6d736d67; h[6] = 0x80000000; h[7] = ssz; h[8] = 0;
            h[9] = 0; h[10] = sp->format; h[11] = sp->width; h[12] = sp->height;
            if (s) { H1[j] = (native_handle_t *)h; F1[j] = bfd; }
            else   { H0[j] = (native_handle_t *)h; F0[j] = bfd; }
        }
    }

    g_vid_dump = 1; g_vid_seq = 0; g_vid_seen = 0; g_vid_step = 1;
    g_vid_stream = &S0; g_vid_width = sz0 / 2; g_vid_height = 1;
    g_vid_stream2 = &S1; g_vid_width2 = sz1 / 2; g_vid_height2 = 1;

    for (int f = 0; f < nreq; f++) {
        camera3_stream_buffer_t ob[2];
        native_handle_t **s0 = (native_handle_t **)malloc(sizeof(void *));
        native_handle_t **s1 = (native_handle_t **)malloc(sizeof(void *));
        *s0 = H0[f % 8]; *s1 = H1[f % 8];
        memset(ob, 0, sizeof(ob));
        ob[0].stream = &S0; ob[0].buffer = s0; ob[0].status = 0;
        ob[0].acquire_fence = -1; ob[0].release_fence = -1;
        ob[1].stream = &S1; ob[1].buffer = s1; ob[1].status = 0;
        ob[1].acquire_fence = -1; ob[1].release_fence = -1;
        camera3_capture_request_t req;
        memset(&req, 0, sizeof(req));
        req.frame_number = f + 1; req.settings = reqmeta;
        req.input_buffer = 0; req.output_buffers = ob; req.num_output_buffers = 2;
        rc = ops->process_capture_request(c3, &req);
        if (rc != 0) { LLOG("TOF2_REQFAIL", "frame=%d rc=%d", f + 1, rc); break; }
        n_submitted++;
        usleep(200000);
    }
    for (int w = 0; w < 300 && n_buffers_back < n_submitted * 2; w++) usleep(20000);
    LLOG("TOF2_WAITDONE", "submitted=%d results=%d buffers_back=%d", n_submitted, n_results, n_buffers_back);
    do_flush_close(c3, ops, dev);
    for (int j = 0; j < 8; j++) { free(H0[j]); free(H1[j]); if (F0[j] > 0) close(F0[j]); if (F1[j] > 0) close(F1[j]); }
    close(ion);
    g_vid_dump = 0; g_vid_width = 0; g_vid_height = 0; g_vid_stream = &g_st1;
    g_vid_stream2 = 0; g_vid_width2 = 0; g_vid_height2 = 0;
    LLOG("TOF2_END", "rc=%d", rc);
    return rc;
}

static int run_4k60(camera_module_t *cam, int camid, int nreq, int dump) {
    static camera3_stream_t S[5];
    camera3_stream_t *sarr[5];
    char idstr[8]; int il = 0, iv = camid;
    if (iv == 0) idstr[il++] = '0';
    else { char tm[8]; int k = 0; while (iv) { tm[k++] = '0' + iv % 10; iv /= 10; } while (k) idstr[il++] = tm[--k]; }
    idstr[il] = 0;
    hw_device_t *dev = 0;
    int rc;

    LLOG("V60_TRY", "cam%d %d frames", camid, nreq);
    rc = cam->common.methods->open(cam, idstr, (void **)&dev);
    LLOG("V60_OPENED", "rc=%d", rc);
    if (rc != 0 || !dev) return 1;
    camera3_device_t *c3 = (camera3_device_t *)dev;
    camera3_device_ops_t *ops = (camera3_device_ops_t *)c3->ops;
    rc = ops->initialize(c3, &callbacks);
    LLOG("V60_INIT", "rc=%d", rc);

    /* Samsung framework-equivalent rawParam via the vendor ops extension slot
     * (ops+0x28 = reserved[2] = CamX::set_parameters). Stock 4K60 sends:
     *   samsungcamera=true; shootingmode=3; recording-fps=60
     * This is what marks the session as a Samsung 4K60 video session. */
    {
        typedef int (*set_params_fn)(const camera3_device_t *, const char *);
        set_params_fn sp = (set_params_fn)ops->reserved[2];
        LLOG("V60_OPS", "ops=%p [0]=%p [10]=set_params %p", ops, ((void**)ops)[0], (void*)sp);
        if (sp) {
            const char *raw = "first-entrance=false;samsungcamera=true;factorytest=false;"
                              "shootingmode=3;recording-fps=60;sw-vdis=false;"
                              "video-beautyface=false;vtmode=0;operation_mode=none;"
                              "ssm_shot_mode=0;recording_dr_mode=sdr;sw-super_vdis=false;stream_type=0;";
            int sprc = sp(c3, raw);
            LLOG("V60_SETPARAMS", "rc=%d rawParam sent", sprc);
        } else {
            LLOG("V60_SETPARAMS", "slot empty — cannot mark Samsung session");
        }
    }

    /* stream table — exactly the stock app's 4K60 session */
    memset(S, 0, sizeof(S));
    /* Unihal-level ground truth (stock logcat UniHal3InterfaceEntry, 4K60 session):
       [0] 1920x1080 0x22 ds 0        usage 0x900   option 0x1
       [1] 3840x2160 0x22 ds 0x104    usage 0x10000 option 0x41
       [2] 1920x1080 0x23 ds 0x8C20000 usage 0x3    option 0x1
       [3] 4032x2268 0x21 ds 0x8C20000 usage 0x3    option 0x2
       [4] 4032x2268 0x23 ds 0x8C20000 usage 0x3    option 0x2 */
    S[0].stream_type = 0; S[0].width = 1920; S[0].height = 1080;
    S[0].format = 0x22; S[0].usage = 0x900; S[0].max_buffers = 10;
    S[1].stream_type = 0; S[1].width = 3840; S[1].height = 2160;
    S[1].format = 0x22; S[1].usage = 0x10000; S[1].max_buffers = 8;
    S[2].stream_type = 0; S[2].width = 1920; S[2].height = 1080;
    S[2].format = 0x23; S[2].usage = 3; S[2].max_buffers = 10;
    S[3].stream_type = 0; S[3].width = 4032; S[3].height = 2268;
    S[3].format = 0x21; S[3].usage = 3; S[3].max_buffers = 8;
    S[4].stream_type = 0; S[4].width = 4032; S[4].height = 2268;
    S[4].format = 0x23; S[4].usage = 3; S[4].max_buffers = 8;
    S[1].data_space = 0x104;
    S[2].data_space = 0x8c20000; S[3].data_space = 0x8c20000;
    S[4].data_space = 0x8c20000;
    /* Samsung streamOption @0x28: 1=preview, 65=record(4K60), 2=photo, from stock app */
    S[0].dynamic_range_profile = 1;
    S[1].dynamic_range_profile = 65;
    S[2].dynamic_range_profile = 1;
    S[3].dynamic_range_profile = 2;
    S[4].dynamic_range_profile = 2;
    for (int i = 0; i < 5; i++) S[i].samsung_ext[0] = 0;   /* ext stays zero (matches real path) */
    for (int i = 0; i < 5; i++) sarr[i] = &S[i];
    g_vid_stream = &S[1];

    camera3_stream_configuration_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_streams = 5; cfg.streams = sarr;

    /* Samsung session parameters — replicated from the stock 4K60 session
     * (dumpsys last-request values; shootingMode=3 = VIDEO is the key the
     * CHI usecase matcher reads; prior attempts never set it) */
    {
        void *sp = allocate_camera_metadata(24, 4096);
        unsigned char one = 1, zero_b = 0;
        int sm = 3, m60 = 60, z0 = 0, ci = 3;
        int fr[2] = { 60, 60 };
        add_camera_metadata_entry(sp, 0x805b0037, &one, 1);   /* samsungCamera=1 */
        add_camera_metadata_entry(sp, 0x805b000d, &sm, 1);    /* shootingMode=3 VIDEO */
        add_camera_metadata_entry(sp, 0x805b0050, &m60, 1);   /* recordingMaxFps=60 */
        add_camera_metadata_entry(sp, 0x805b0051, &zero_b, 1);
        add_camera_metadata_entry(sp, 0x805b0052, &zero_b, 1);
        add_camera_metadata_entry(sp, 0x805b0053, &zero_b, 1);
        add_camera_metadata_entry(sp, 0x805b0054, &z0, 1);
        add_camera_metadata_entry(sp, 0x805b0055, &zero_b, 1);
        add_camera_metadata_entry(sp, 0x80610009, &z0, 1);    /* sensor.streamType=0 */
        add_camera_metadata_entry(sp, 0x805b005d, &z0, 1);    /* cameraClient=0 */
        add_camera_metadata_entry(sp, 0x805b0060, &z0, 1);    /* avspConnected=0 */
        add_camera_metadata_entry(sp, 0x0001000d, &ci, 1);    /* captureIntent=VIDEO_RECORD */
        add_camera_metadata_entry(sp, 0x00010005, fr, 2);     /* aeTargetFpsRange [60,60] */
        add_camera_metadata_entry(sp, 0x805b0034, &ci, 1);    /* captureMode=3 */
        add_camera_metadata_entry(sp, 0x805b0038, &one, 1);   /* recordingHint=1 */
        add_camera_metadata_entry(sp, 0x800b000a, &m60, 1);   /* targetFPS=60 */
        cfg.session_parameters = sp;
        LLOG("V60_SESSPARAMS", "%p", sp);
    }
    const void *reqmeta = ops->construct_default_request_settings(c3, 1 /*PREVIEW*/);
    LLOG("V60_DEFAULTS", "%p", reqmeta);

    /* clone request + apply all overrides (fps 60 etc.), poke CHI global */
    {
        size_t cnt = get_camera_metadata_entry_count(reqmeta);
        void *dst = allocate_camera_metadata(cnt + 32, 262144);
        size_t copied = 0;
        for (size_t i = 0; i < cnt; i++) {
            ro_entry_t e; memset(&e, 0, sizeof(e));
            if (get_camera_metadata_entry(reqmeta, i, &e) != 0) continue;
            if (e.tag == 0x00010005 || e.tag == 0x000e0001 || e.tag == 0x0001000d) continue;
            const void *dptr = e.data_ptr ? e.data_ptr : (const void *)&e.data;
            if (add_camera_metadata_entry(dst, e.tag, dptr, e.count) == 0) copied++;
        }
        int fr[2] = { 60, 60 };
        add_camera_metadata_entry(dst, 0x00010005, fr, 2);
        long long fdv = 16666666LL;
        add_camera_metadata_entry(dst, 0x000e0001, &fdv, 1);
        unsigned char ci = 3;
        add_camera_metadata_entry(dst, 0x0001000d, &ci, 1);
        int sm3 = 3;
        add_camera_metadata_entry(dst, 0x805b000d, &sm3, 1);  /* shootingMode=VIDEO */
        int mv = 60;
        add_camera_metadata_entry(dst, 0x805b0050, &mv, 1);   /* recordingMaxFps */
        add_camera_metadata_entry(dst, 0x800b000a, &mv, 1);   /* targetFPS */
        unsigned char hv = 1;
        add_camera_metadata_entry(dst, 0x805b0038, &hv, 1);   /* recordingHint */
        int cm = 3;
        add_camera_metadata_entry(dst, 0x805b0034, &cm, 1);   /* captureMode */
        reqmeta = dst;
        LLOG("V60_REQCLONE", "copied=%zu", copied);
    }
    {
        unsigned long base = get_chi_base();
        if (base) {
            volatile unsigned int *p = (unsigned int *)(base + 0x2772EC);
            unsigned int old = *p;
            *p = 60;
            LLOG("V60_POKE", "chi@0x%lx fps-global: %u -> 60", base, old);
        }
    }

    /* hex dump the full stream structs (0x68 bytes each — Camera3Stream wrapper size) */
    for (int si = 0; si < 5; si++) {
        uint32_t *sw = (uint32_t *)&S[si];
        printf("[stream%d]", si);
        for (int wi = 0; wi < 26; wi++) printf(" %08x", sw[wi]);
        printf("\n");
    }
    rc = ops->configure_streams(c3, &cfg);
    LLOG("V60_CONFIGURED", "rc=%d", rc);
    if (rc != 0) { do_flush_close(c3, ops, dev); return 2; }

    /* buffers per stream: counts [2,8,2,1,1]; blob stream gets 16MB */
    static native_handle_t *H[5][8];
    static int F[5][8];
    uint32_t sz[5];
    int cnts[5] = { 2, 8, 2, 1, 1 };
    int ion = open("/dev/ion", 0);
    for (int i = 0; i < 5; i++) {
        sz[i] = (S[i].format == 0x21) ? 16 * 1024 * 1024 : S[i].width * S[i].height * 2;
        for (int j = 0; j < cnts[i]; j++) {
            struct ion_alloc_414 ia; memset(&ia, 0, sizeof(ia));
            ia.len = sz[i]; ia.heap_mask = 0x2000000; ia.flags = 0;
            if (ioctl(ion, ION_IOC_ALLOC_414, &ia) != 0) { printf("[shim] v60 alloc %d/%d failed\n", i, j); return 3; }
            int bfd = (int)ia.fd;
            int *h = (int *)malloc(sizeof(int) * 28);
            memset(h, 0, sizeof(int) * 28);
            h[0] = 12; h[1] = 2; h[2] = 23; h[3] = bfd; h[4] = -1;
            h[5] = 0x6d736d67; h[6] = 0x80000000; h[7] = sz[i]; h[8] = 0;
            h[9] = 0; h[10] = S[i].format; h[11] = S[i].width; h[12] = S[i].height;
            H[i][j] = (native_handle_t *)h; F[i][j] = bfd;
        }
    }

    /* requests: 1 buffer per stream — but ONLY the 3 real-time streams
     * (preview, record, previewCb) like the stock app's repeating request;
     * the photo streams (BLOB/YUV) stay configured but are not requested
     * per-frame (stock requests them only for still-capture-during-video). */
    for (int f = 0; f < nreq; f++) {
        camera3_stream_buffer_t ob[3];
        native_handle_t **slots[3];
        memset(ob, 0, sizeof(ob));
        for (int i = 0; i < 3; i++) {
            ob[i].stream = &S[i];
            slots[i] = (native_handle_t **)malloc(sizeof(void *));
            *slots[i] = H[i][f % cnts[i]];
            ob[i].buffer = slots[i];
            ob[i].status = 0; ob[i].acquire_fence = -1; ob[i].release_fence = -1;
        }
        camera3_capture_request_t req;
        memset(&req, 0, sizeof(req));
        req.frame_number = f + 1;
        req.settings = reqmeta;
        req.input_buffer = 0;
        req.output_buffers = ob;
        req.num_output_buffers = 3;
        req.input_width = 0; req.input_height = 0;
        LLOG("V60_SUBMIT", "frame=%d", f + 1);
        rc = ops->process_capture_request(c3, &req);
        if (rc != 0) { LLOG("V60_REQFAIL", "frame=%d rc=%d", f + 1, rc); break; }
        n_submitted++;
        if (f > 0) usleep(8000);   /* pace a little */
    }
    for (int w = 0; w < 300 && n_buffers_back < n_submitted * 2; w++) usleep(20000);
    LLOG("V60_WAITDONE", "submitted=%d results=%d buffers_back=%d", n_submitted, n_results, n_buffers_back);

    do_flush_close(c3, ops, dev);

    for (int i = 0; i < 5; i++)
        for (int j = 0; j < cnts[i]; j++) {
            free(H[i][j]);
            if (F[i][j] > 0) close(F[i][j]);
        }
    close(ion);
    g_vid_stream = &g_st1;
    LLOG("V60_END", "rc=%d", rc);
    return rc;
}

int shim_main(int argc, char **argv, char **envp) {
    g_argc = argc; g_argv = argv;
    setvbuf(stdout, (void *)0, 2, 0);
    const char *path = "/vendor/lib/hw/camera.qcom.so";
    void *h = dlopen(path, 2);
    if (!h) { printf("[shim] dlopen FAILED: %s\n", dlerror()); return 1; }
    printf("[shim] dlopen ok\n");
    hw_module_t *mod = (hw_module_t *)dlsym(h, "HMI");
    if (!mod) { printf("[shim] no HMI: %s\n", dlerror()); return 1; }
    printf("[shim] HMI: id=%s name=%s\n", mod->id, mod->name);
#ifdef __aarch64__
    {
        /* dump module fn-ptr slots to verify 64-bit layout */
        unsigned long *m = (unsigned long *)mod;
        for (int i = 0; i < 26; i += 2)
            printf("[mod] +0x%02x: 0x%016lx 0x%016lx\n", i * 8, m[i], m[i+1]);
    }
#endif

    camera_module_t *cam = (camera_module_t *)mod;
    if (cam->init) printf("[shim] module init rc=%d\n", cam->init());

    int n = cam->get_number_of_cameras();
    printf("[shim] number_of_cameras = %d\n", n);
    int camid = 0;
    if (argc > 1) { const char *q = argv[1]; while (*q >= '0' && *q <= '9') camid = camid * 10 + (*q++ - '0'); }
    if (!cam->common.methods || !cam->common.methods->open) {
        printf("[shim] no methods->open\n"); return 1;
    }

    /* test mode: argv[2] = a|b|c|e|p|x (default c) */
    char mode = (argc > 2) ? argv[2][0] : 'c';
    int rc = 0;
    switch (mode) {
    case 'a':  /* open/initialize/close only */
        LLOG("TEST_A", "open+init+close");
        rc = run_session2(cam, camid, 0, 0, 4032, 3024, "/data/local/tmp/frame0.raw", 0);
        break;
    case 'b':  /* one capture */
        LLOG("TEST_B", "1 capture");
        rc = run_session2(cam, camid, 1, 0, 4032, 3024, "/data/local/tmp/frame0.raw", 0);
        break;
    case 'e':  /* repeated device lifecycle */
        LLOG("TEST_E", "cycle 1");
        rc = run_session2(cam, camid, 1, 0, 4032, 3024, "/data/local/tmp/frame0.raw", 0);
        flush_called = 0; close_called = 0;   /* new device instance next cycle */
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        LLOG("TEST_E", "cycle 2");
        rc = run_session2(cam, camid, 1, 0, 4032, 3024, "/data/local/tmp/frame0.raw", 0);
        break;
    case 'p': {  /* photo on EVERY module, resolution fallback per module */
        static const struct { uint32_t w, h; } sizes[] = {
            {4032,3024},{4000,3000},{3840,2880},{3648,2736},{3264,2448},
            {2560,1920},{1920,1440},{1920,1080},{1280,960},{1280,720},{640,480}
        };
        for (int id = 0; id < n && id < 5; id++) {
            camera_info_t info; memset(&info, 0, sizeof(info));
            cam->get_camera_info(id, &info);
            LLOG("PHOTO_MODULE", "cam%d facing=%s orientation=%d",
                 id, info.facing == 0 ? "BACK" : (info.facing == 1 ? "FRONT" : "?"),
                 info.orientation);
            char path[32];
            /* path built by hand: /tmp/frameN.raw */
            static const char hexd[] = "0123456789";
            char *q = path; const char *s = "/tmp/frame";
            while (*s) *q++ = *s++;
            *q++ = hexd[id]; s = ".raw";
            while (*s) *q++ = *s++;
            *q = 0;
            for (unsigned si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
                flush_called = 0; close_called = 0;
                n_submitted = n_results = n_buffers_back = n_notify = 0;
                got_frame = 0;
                LLOG("PHOTO_TRY", "cam%d %ux%u", id, sizes[si].w, sizes[si].h);
                rc = run_session2(cam, id, 1, 1, sizes[si].w, sizes[si].h, path, 0);
                if (rc == 0) { LLOG("PHOTO_OK", "cam%d %ux%u -> %s", id, sizes[si].w, sizes[si].h, path); break; }
                LLOG("PHOTO_RETRY", "cam%d %ux%u rc=%d", id, sizes[si].w, sizes[si].h, rc);
            }
        }
        break;
    }
    case 'm': {  /* dump static metadata: physical camera IDs + stream configs */
        int lo = 0, hi = n;
        if (argc > 3) { lo = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') lo = lo * 10 + (*q++ - '0'); hi = lo + 1; }
        for (int id = lo; id < hi; id++) {
            camera_info_t info; memset(&info, 0, sizeof(info));
            int r = cam->get_camera_info(id, &info);
            const void *st = info.static_camera_characteristics;
            size_t cnt = st ? get_camera_metadata_entry_count(st) : 0;
            LLOG("META", "cam%d rc=%d facing=%d orient=%d entries=%d", id, r,
                 info.facing, info.orientation, (int)cnt);
            if (!st) continue;
            for (size_t i = 0; i < cnt; i++) {
                ro_entry_t e;
                memset(&e, 0, sizeof(e));
                if (get_camera_metadata_entry(st, i, &e) != 0) continue;
                if (e.tag == 0x80290000 && e.count > 4) {  /* SensorModeTable: dump fully */
                    const int *v = (const int *)e.data_ptr;
                    printf("[mode-table] cam%d", id);
                    for (unsigned k = 0; k < e.count; k++) printf(" %d", v[k]);
                    printf("\n");
                }
                if (id == 0 || e.tag >= 0x80000000) {
                    /* compact: tag, type, count, up to 4 first values */
                    printf("[raw] cam%d tag=0x%08x type=%d count=%d", id, e.tag, e.type, e.count);
                    if (e.count >= 1) {
                        if (e.type == 1 && e.data_ptr) {
                            const int *v = (const int *)e.data_ptr;
                            for (unsigned k = 0; k < 4 && k < e.count; k++) printf(" i%d=%d", k, v[k]);
                        } else if (e.type == 0 && e.data_ptr) {
                            const unsigned char *b = (const unsigned char *)e.data_ptr;
                            printf(" hx=");
                            for (unsigned k = 0; k < 12 && k < e.count; k++) printf("%02x", b[k]);
                            printf(" as='");
                            for (unsigned k = 0; k < 12 && k < e.count; k++) putchar(b[k] >= 32 && b[k] < 127 ? b[k] : '.');
                            printf("'");
                        } else if (e.count == 1) {
                            printf(" v=%d", e.data.i32);
                        }
                    }
                    printf("\n");
                }
            }
        }
        break;
    }
    case 'q': {  /* capture via a physical camera of the logical camera: argv[3]=phys id */
        const char *phys = (argc > 3) ? argv[3] : "3";
        static const struct { uint32_t w, h; } tsizes[] = {
            {4032,3024},{3840,2880},{3264,2448},{2560,1920},{1920,1080},{1280,720}
        };
        for (unsigned si = 0; si < sizeof(tsizes)/sizeof(tsizes[0]); si++) {
            flush_called = 0; close_called = 0;
            n_submitted = n_results = n_buffers_back = n_notify = 0;
            got_frame = 0;
            LLOG("PHYS_TRY", "cam%d phys='%s' %ux%u", camid, phys, tsizes[si].w, tsizes[si].h);
            rc = run_session2(cam, camid, 1, 1, tsizes[si].w, tsizes[si].h,
                              "/tmp/frame_phys.raw", phys);
            if (rc == 0) { LLOG("PHYS_OK", "cam%d phys='%s' %ux%u", camid, phys,
                               tsizes[si].w, tsizes[si].h); break; }
            LLOG("PHYS_RETRY", "rc=%d", rc);
        }
        break;
    }
    case 'v': {  /* enumerate vendor tags: id + name, filter zoom/physical */
        struct vendor_tag_ops {
            int (*get_tag_count)(const void *);
            void (*get_all_tags)(const void *, uint32_t *);
            const char *(*get_section_name)(const void *, uint32_t);
            const char *(*get_tag_name)(const void *, uint32_t);
            int (*get_tag_type)(const void *, uint32_t);
            void *reserved[8];
        } vt;
        memset(&vt, 0, sizeof(vt));
        if (!cam->get_vendor_tag_ops) { printf("[vt] no get_vendor_tag_ops\n"); break; }
        cam->get_vendor_tag_ops(&vt);
        int tcnt = vt.get_tag_count ? vt.get_tag_count(&vt) : -1;
        LLOG("VENDOR_TAGS", "count=%d", tcnt);
        if (tcnt > 0 && tcnt < 4096) {
            static uint32_t tags[4096];
            vt.get_all_tags(&vt, tags);
            for (int i = 0; i < tcnt; i++) {
                const char *nm = vt.get_tag_name ? vt.get_tag_name(&vt, tags[i]) : 0;
                const char *sc = vt.get_section_name ? vt.get_section_name(&vt, tags[i]) : 0;
                if (!nm) nm = "";
                if (!sc) sc = "";
                /* print all; filter is done by the caller */
                printf("[vt] 0x%08x t=%d %s.%s\n", tags[i], vt.get_tag_type ? vt.get_tag_type(&vt, tags[i]) : -1, sc, nm);
            }
        }
        break;
    }
    case 'd': {  /* dual physical capture: preview=phys "0", capture=phys argv[3] */
        const char *pp = (argc > 3 && argv[3][0] != 'x') ? argv[3] : "3";
        g_fmt0 = g_fmt1 = 0x22; g_use0 = g_use1 = 0x102;
        g_preview_physid = "0";
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("DUAL_TRY", "preview_phys=0 capture_phys=%s", pp);
        rc = run_session2(cam, camid, 1, 1, 1440, 1080, "/data/local/tmp/frame_dual.raw", pp);
        LLOG("DUAL_DONE", "rc=%d", rc);
        g_fmt0 = g_fmt1 = 0x23; g_use0 = g_use1 = 0;
        g_preview_physid = 0;
        break;
    }
    case 'w': {  /* multicam capture (0x22 impl-defined streams): argv[3]=phys|x argv[4]=zoom|x */
        g_poke_fps = 60;  /* ALWAYS poke FPS to 60 in mode w */
        g_sensor_mode = 8;  /* force sensor mode 8: 4032x2268 @ 60fps */
        g_fps_range = 60;   /* build session params with fps 60 + sensor mode */
        int phys = -1;
        if (argc > 3 && argv[3][0] != 'x') { phys = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') phys = phys * 10 + (*q++ - '0'); }
        g_capture_phys = phys;
        if (argc > 4 && argv[4][0] != 'x') {
            int ip = 0; const char *q = argv[4];
            while (*q >= '0' && *q <= '9') ip = ip * 10 + (*q++ - '0');
            int frac = 0, mul = 1;
            if (*q == '.') { q++; while (*q >= '0' && *q <= '9') { frac = frac * 10 + (*q++ - '0'); mul *= 10; } }
            g_zoom_x100 = ip * 100 + frac * 100 / mul;
        }
        if (argc > 5 && argv[5][0] != 'x') {
            g_samsung_cam = 0; const char *q = argv[5];
            while (*q >= '0' && *q <= '9') g_samsung_cam = g_samsung_cam * 10 + (*q++ - '0');
        }
        if (argc > 6 && argv[6][0] != 'x') {
            g_capture_mode = 0; const char *q = argv[6];
            while (*q >= '0' && *q <= '9') g_capture_mode = g_capture_mode * 10 + (*q++ - '0');
        }
        if (argc > 7 && argv[7][0] != 'x') {
            int ip = 0; const char *q = argv[7];
            while (*q >= '0' && *q <= '9') ip = ip * 10 + (*q++ - '0');
            int frac = 0, mul = 1;
            if (*q == '.') { q++; while (*q >= '0' && *q <= '9') { frac = frac * 10 + (*q++ - '0'); mul *= 10; } }
            g_crop_x100 = ip * 100 + frac * 100 / mul;
        }
        g_fmt0 = 0x22; g_use0 = 0x102;      /* preview: IMPLEMENTATION_DEFINED, HW_TEXTURE|SW_READ_RARELY */
        g_fmt1 = 0x22; g_use1 = 0x102;      /* capture: same class (advanced usecase) */
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("MCCAP_TRY", "phys=%d zoom=%d", g_capture_phys, g_zoom_x100);
        rc = run_session2(cam, camid, 1, 1, 4032, 3024, "/data/local/tmp/frame_mc.raw", 0);
        LLOG("MCCAP_DONE", "rc=%d", rc);
        g_fmt0 = g_fmt1 = 0x23; g_use0 = g_use1 = 0;
        g_capture_phys = -1; g_zoom_x100 = -1; g_samsung_cam = -1; g_capture_mode = -1; g_crop_x100 = -1;
        break;
    }
    case 'V': {  /* video: preview + encoder stream, 60 frames dumped as vidNN.raw */
        g_fmt0 = 0x22; g_use0 = 0x20900;   /* preview: impl-defined, camera|composer|texture */
        g_fmt1 = 0x22; g_use1 = 0x30000;   /* video: impl-defined, encoder|camera-write */
        g_vid_dump = 1; g_vid_seq = 0;
        g_req_delay_ms = 0;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("VIDEO_TRY", "cam%d 1920x1080 60 frames", camid);
        rc = run_session2(cam, camid, 60, 0, 1920, 1080, "/data/local/tmp/frame_vid.raw", 0);
        LLOG("VIDEO_DONE", "rc=%d buffers_back=%d", rc, n_buffers_back);
        g_fmt0 = g_fmt1 = 0x23; g_use0 = g_use1 = 0;
        g_vid_dump = 0;
        break;
    }
    case 'A': {  /* aperture watch with afMode override: argv[3]=afMode (3=CONTINUOUS_VIDEO) */
        int m = 3;
        if (argc > 3) { m = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') m = m * 10 + (*q++ - '0'); }
        g_af_mode = m;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("AFAPER_TRY", "cam%d afMode=%d", camid, m);
        rc = run_session2(cam, camid, 80, 0, 4032, 3024, "/data/local/tmp/frame_afaper.raw", 0);
        LLOG("AFAPER_DONE", "rc=%d", rc);
        g_af_mode = -1;
        break;
    }
    case 'L': {  /* light-condition injection: argv[3]=lightcond, argv[4]=apex*10 */
        int lc = 1, ax = 140;
        if (argc > 3) { lc = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') lc = lc * 10 + (*q++ - '0'); }
        if (argc > 4) { ax = 0; int neg = 0; const char *q = argv[4]; if (*q == '-') { neg = 1; q++; } while (*q >= '0' && *q <= '9') ax = ax * 10 + (*q++ - '0'); if (neg) ax = -ax; }
        g_lightcond = lc; g_apex_x10 = ax;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("LIGHT_TRY", "cam%d lightcond=%d apex=%d", camid, lc, ax);
        rc = run_session2(cam, camid, 15, 0, 4032, 3024, "/data/local/tmp/frame_light.raw", 0);
        LLOG("LIGHT_DONE", "rc=%d", rc);
        g_lightcond = -1; g_apex_x10 = -1;
        break;
    }
    case 'O': {  /* OIS test: argv[3]=mode (1=on), capture 10 frames dumping results */
        int m = 1;
        if (argc > 3) { m = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') m = m * 10 + (*q++ - '0'); }
        g_ois_mode = m;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("OIS_TRY", "cam%d mode=%d", camid, m);
        rc = run_session2(cam, camid, 10, 1, 4032, 3024, "/data/local/tmp/frame_ois.raw", 0);
        LLOG("OIS_DONE", "rc=%d", rc);
        g_ois_mode = -1;
        break;
    }
    case 'H': {  /* 4K60 video: 3840x2160 encoder stream, AE fps 60, sensor mode argv[3] */
        int smi = 8;
        if (argc > 3) { smi = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') smi = smi * 10 + (*q++ - '0'); }
        int stp = 2;
        if (argc > 4) { stp = 0; const char *q = argv[4]; while (*q >= '0' && *q <= '9') stp = stp * 10 + (*q++ - '0'); }
        uint32_t rw = 3840, rh = 2160;
        if (argc > 5) { rw = 0; const char *q = argv[5]; while (*q >= '0' && *q <= '9') rw = rw * 10 + (*q++ - '0'); }
        if (argc > 6) { rh = 0; const char *q = argv[6]; while (*q >= '0' && *q <= '9') rh = rh * 10 + (*q++ - '0'); }
        if (rw < 640 || rh < 480) { rw = 3840; rh = 2160; }
        if (argc > 7) { g_use_3rd = 0; const char *q = argv[7]; while (*q >= '0' && *q <= '9') g_use_3rd = g_use_3rd * 10 + (*q++ - '0'); }
        g_fmt0 = 0x22; g_use0 = 0x20900;
        g_fmt1 = 0x22; g_use1 = 0x30000;
        g_prev_w = 1920; g_prev_h = 1080;   /* stock 4K60 uses 1920x1080 preview */
        g_recording_max_fps = 60; g_recording_hint = 1; g_capture_mode_hint = 3;
        g_cap_buffers = 4; g_use_3rd = 1; g_poke_fps = 60;
        g_vid_dump = 1; g_vid_seq = 0; g_vid_seen = 0; g_vid_step = stp;
        g_fps_range = 60;
        g_sensor_mode = smi;
        g_req_delay_ms = 0;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("VIDEO4K_TRY", "cam%d %ux%u fps60", camid, rw, rh);
        rc = run_session2(cam, camid, 60, 0, rw, rh, "/data/local/tmp/frame_4k.raw", 0);
        LLOG("VIDEO4K_DONE", "rc=%d buffers_back=%d", rc, n_buffers_back);
        g_fmt0 = g_fmt1 = 0x23; g_use0 = g_use1 = 0;
        g_vid_dump = 0; g_vid_step = 1; g_fps_range = 0; g_sensor_mode = -1;
        g_prev_w = 1440; g_prev_h = 1080; g_recording_max_fps = -1; g_recording_hint = -1;
        g_capture_mode_hint = -1; g_cap_buffers = 4; g_req_clone = 0; g_use_3rd = 0; g_af_mode = -1; g_poke_fps = 0;
        break;
    }
    case '9': {  /* 4K60 stock-replica: argv[3]=frames argv[4]=poke(1)/nopoke(0) */
        int nf = 90;
        if (argc > 3) { nf = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') nf = nf * 10 + (*q++ - '0'); }
        if (nf < 1) nf = 1;
        if (argc > 4 && argv[4][0] == '0') g_poke_fps = 0; else g_poke_fps = 60;
        g_vid_dump = 1; g_vid_seq = 0; g_vid_seen = 0; g_vid_step = 1;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        rc = run_4k60(cam, camid, nf, 1);
        g_vid_dump = 0; g_poke_fps = 0;
        break;
    }
    case 't': {  /* ToF depth capture: t [w h fmt [nframes]]; nframes handled inside run_tof */
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0; g_tof_laser_on = 0;
        rc = run_tof(cam, camid, 8);
        break;
    }
    case 'T': {  /* 2-stream ToF: T w1 h1 fmt2 w2 h2 [nframes] */
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        rc = run_tof2(cam, camid, 8);
        break;
    }
    case 'j': {  /* slow aperture test: hold session, frames argv[3] (default 40) @200ms */
        int nf = 40;
        if (argc > 3) { nf = 0; const char *q = argv[3]; while (*q >= '0' && *q <= '9') nf = nf * 10 + (*q++ - '0'); }
        if (nf < 4) nf = 4;
        g_aperture = -1;
        g_req_delay_ms = 200;
        g_dump_every = 10;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("SLOW_TRY", "cam%d 40 frames 200ms apart", camid);
        rc = run_session2(cam, camid, nf, 0, 4032, 3024, "/data/local/tmp/frame_slow.raw", 0);
        g_req_delay_ms = 0; g_dump_every = 0;
        LLOG("SLOW_DONE", "rc=%d", rc);
        break;
    }
    case 'i': {  /* aperture capture: argv[3]=SS_AEC_APERTURECONTROL value, request camid arg */
        int av = 0;
        if (argc > 3) { const char *q = argv[3]; while (*q >= '0' && *q <= '9') av = av * 10 + (*q++ - '0'); }
        g_aperture = av;
        flush_called = 0; close_called = 0;
        n_submitted = n_results = n_buffers_back = n_notify = 0;
        got_frame = 0;
        LLOG("APER_TRY", "cam%d SS_AEC_APERTURECONTROL=%d", camid, av);
        rc = run_session2(cam, camid, 6, 1, 4032, 3024, "/data/local/tmp/frame_aper.raw", 0);
        LLOG("APER_DONE", "rc=%d", rc);
        g_aperture = -1;
        break;
    }
    case 'z': {  /* capture with samsung controls: argv[3]=phys|x argv[4]=zoom|x argv[5]=samscam|x */
        int phys = -1;
        if (argc > 3 && argv[3][0] != 'x') {
            phys = 0; const char *p = argv[3];
            while (*p >= '0' && *p <= '9') phys = phys * 10 + (*p++ - '0');
        }
        g_capture_phys = phys;
        if (argc > 4 && argv[4][0] != 'x') {
            int ip = 0; const char *p = argv[4];
            while (*p >= '0' && *p <= '9') ip = ip * 10 + (*p++ - '0');
            int frac = 0, mul = 1;
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { frac = frac * 10 + (*p++ - '0'); mul *= 10; } }
            g_zoom_x100 = ip * 100 + frac * 100 / mul;
        }
        if (argc > 5 && argv[5][0] != 'x') {
            g_samsung_cam = 0; const char *p = argv[5];
            while (*p >= '0' && *p <= '9') g_samsung_cam = g_samsung_cam * 10 + (*p++ - '0');
        }
        LLOG("ZPARAMS", "phys=%d zoom_x100=%d samscam=%d", g_capture_phys, g_zoom_x100, g_samsung_cam);
        static const struct { uint32_t w, h; } zs[] = {
            {4032,3024},{3840,2880},{3264,2448},{1920,1080}
        };
        for (unsigned si = 0; si < sizeof(zs)/sizeof(zs[0]); si++) {
            flush_called = 0; close_called = 0;
            n_submitted = n_results = n_buffers_back = n_notify = 0;
            got_frame = 0;
            LLOG("CAPPHYS_TRY", "cam%d phys=%d zoom=%d samscam=%d %ux%u", camid,
                 g_capture_phys, g_zoom_x100, g_samsung_cam, zs[si].w, zs[si].h);
            rc = run_session2(cam, camid, 1, 1, zs[si].w, zs[si].h, "/data/local/tmp/frame_capphys.raw", 0);
            if (rc == 0) { LLOG("CAPPHYS_OK", "phys=%d zoom=%d %ux%u", g_capture_phys,
                                g_zoom_x100, zs[si].w, zs[si].h); break; }
            LLOG("CAPPHYS_RETRY", "rc=%d", rc);
        }
        g_capture_phys = -1; g_zoom_x100 = -1; g_samsung_cam = -1;
        break;
    }
    case 's': {  /* configure arbitrary streams: argv[3]="fmt:w:h;fmt:w:h;..." then close */
        const char *spec = (argc > 3) ? argv[3] : "0x23:1440:1080;0x23:4032:3024";
        static camera3_stream_t sstr[3];
        static camera3_stream_t *sptr[3];
        int nst = 0;
        const char *p = spec;
        while (*p && nst < 3) {
            int fmt = 0, w = 0, h = 0;
            if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) { p += 2; while (*p) { int c=*p; int d=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; if(d<0)break; fmt=fmt*16+d; p++; } }
            else { while (*p>='0'&&*p<='9') { fmt=fmt*10+(*p++-'0'); } }
            if (*p==':') p++;
            while (*p>='0'&&*p<='9') { w=w*10+(*p++-'0'); }
            if (*p==':') p++;
            while (*p>='0'&&*p<='9') { h=h*10+(*p++-'0'); }
            unsigned usage = 0;
            if (*p==':') {
                p++;
                if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) {
                    p += 2;
                    while (*p) { int c=*p; int d=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; if(d<0)break; usage=usage*16+d; p++; }
                } else {
                    while (*p>='0'&&*p<='9') { usage=usage*10+(*p++-'0'); }
                }
            }
            /* optional 5th field: physical camera id string */
            const char *pid = 0;
            if (*p==':') {
                p++;
                static char pidbuf[3][8];
                int k = 0;
                while (*p && *p != ';' && k < 7) pidbuf[nst][k++] = *p++;
                pidbuf[nst][k] = 0;
                pid = pidbuf[nst];
            }
            if (*p==';') p++;
            memset(&sstr[nst], 0, sizeof(sstr[nst]));
            if (fmt & 0x80000000) {  /* bit31: SecCamTOF depth data_space=0x1000 */
                fmt &= 0x7fffffff;
                sstr[nst].data_space = 0x1000;
            }
            sstr[nst].stream_type = 0;
            sstr[nst].width = w; sstr[nst].height = h;
            sstr[nst].format = fmt; sstr[nst].usage = usage; sstr[nst].max_buffers = 4;
            sstr[nst].physical_camera_id = pid;
            sptr[nst] = &sstr[nst];
            nst++;
        }
        char idstr[8]; int il = 0, iv = camid;
    if (iv == 0) idstr[il++] = '0';
    else { char tm[8]; int k = 0; while (iv) { tm[k++] = '0' + iv % 10; iv /= 10; } while (k) idstr[il++] = tm[--k]; }
    idstr[il] = 0;
        hw_device_t *dv = 0;
        int orc = cam->common.methods->open(cam, idstr, (void **)&dv);
        LLOG("SCFG_OPEN", "rc=%d", orc);
        if (orc == 0 && dv) {
            camera3_device_t *c3 = (camera3_device_t *)dv;
            camera3_device_ops_t *ops = (camera3_device_ops_t *)c3->ops;
            ops->initialize(c3, &callbacks);
            camera3_stream_configuration_t cfg2;
            memset(&cfg2, 0, sizeof(cfg2));
            cfg2.num_streams = nst; cfg2.streams = sptr;
            cfg2.operation_mode = 0;
            cfg2.session_parameters = 0;
            if (g_zoom_x100 >= 0 || g_samsung_cam >= 0)
                cfg2.session_parameters = build_session_params(g_zoom_x100);
            LLOG("SCFG", "n=%d", nst);
            for (int q = 0; q < nst; q++)
                printf("[scfg]   fmt=0x%x %ux%u usage=0x%x phys=%s\n", sstr[q].format, sstr[q].width,
                       sstr[q].height, sstr[q].usage, sstr[q].physical_camera_id ? sstr[q].physical_camera_id : "-");
            int crc = ops->configure_streams(c3, &cfg2);
            LLOG("SCFG_RESULT", "streams=%d rc=%d", nst, crc);
            if (crc == 0) ops->flush(c3);
            dv->close(dv);
        }
        rc = 0;
        break;
    }
    case 'o': {  /* probe arbitrary camera id (no clamping): open+info+close */
        int id = 0;
        if (argc > 3) { const char *p = argv[3]; while (*p >= '0' && *p <= '9') id = id * 10 + (*p++ - '0'); }
        else id = camid;
        char idstr[8]; int n2 = 0, iv = id;
        if (iv == 0) idstr[n2++] = '0';
        else { char tmp[8]; int k = 0; while (iv) { tmp[k++] = '0' + iv % 10; iv /= 10; } while (k) idstr[n2++] = tmp[--k]; }
        idstr[n2] = 0;
        camera_info_t inf; memset(&inf, 0, sizeof(inf));
        int ir = cam->get_camera_info(id, &inf);
        hw_device_t *dv = 0;
        int orc = cam->common.methods->open(cam, idstr, (void **)&dv);
        LLOG("PROBE_ID", "id=%s info_rc=%d facing=%d open_rc=%d dev=%p ver=0x%x",
             idstr, ir, inf.facing, orc, dv, dv ? dv->version : 0);
        if (orc == 0 && dv) { dv->close(dv); }
        rc = 0;
        break;
    }
    case 'n': {  /* dump standard tag table names */
        for (uint32_t t = 0; t <= 0x00100000; t++) {
            const char *nm = get_camera_metadata_tag_name(t);
            if (nm) printf("[tag] 0x%08x t=%d %s\n", t, get_camera_metadata_tag_type(t), nm);
        }
        break;
    }
    case 'x':  /* REPRO: old broken behavior — 4 captures, NO flush/close */
        LLOG("TEST_X", "4 captures, no flush/close (abort repro)");
        flush_called = 1; close_called = 1;   /* makes do_flush_close a no-op */
        rc = run_session2(cam, camid, 4, 0, 4032, 3024, "/data/local/tmp/frame0.raw", 0);
        break;
    case 'c':  /* four captures (original workflow) */
    default:
        LLOG("TEST_C", "4 captures");
        rc = run_session2(cam, camid, 4, 1, 4032, 3024, "/data/local/tmp/frame0.raw", 0);
        break;
    }

    LLOG("PROCESS_EXIT", "rc=%d submitted=%d results=%d buffers_back=%d flush=%d close=%d",
         rc, n_submitted, n_results, n_buffers_back, flush_called, close_called);
    printf("[shim] DONE\n");

    if (mode != 'x') {
        /* Full Camera3 lifecycle is complete (flush+close, all results delivered,
         * shim resources freed). CamX spawns immortal singleton worker threads
         * (16 still alive 5s after close; CamX logs "job not Unregistered" at close)
         * and its static destructors race them at exit -> Scudo heap-corruption abort.
         * Stock Android never runs these destructors either: init SIGKILLs
         * cameraserver/provider. Terminate the way the platform does. */
        extern void _exit(int);
        _exit(rc);
    }
    return rc;   /* mode 'x': legacy broken path, kept for forensics */
}

#if defined(__aarch64__)
__attribute__((naked)) void _start(void) {
    __asm__ volatile(
        "mov x0, sp\n"
        "mov x1, xzr\n"
        ".Lbase:\n"
        " adr x2, shim_main\n"
        " adr x3, structors_\n"
        " bl __libc_init\n"
        " brk #0\n");
}
#else
__attribute__((naked)) void _start(void) {
    __asm__ volatile(
        "mov r0, sp\n"
        "mov r1, #0\n"
        ".Lbase:\n"
        " adr r2, .Lbase\n"
        " ldr r3, .Loff_main\n"
        " add r2, r2, r3\n"
        " adr r3, .Lbase\n"
        " ldr r12, .Loff_struct\n"
        " add r3, r3, r12\n"
        " bl __libc_init\n"
        " bkpt #0\n"
        ".Loff_main: .word shim_main - .Lbase\n"
        ".Loff_struct: .word structors_ - .Lbase\n");
}
#endif
