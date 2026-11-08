/* ABI layout probe — 32-bit ARM, compiled against actual vendor-era headers.
 * Verifies the Samsung 32-bit camera3 ABI derived from binary analysis:
 *   camera3_stream_t     = 0x68 (Samsung extension; AOSP base is 0x40)
 *   streamOption         @ 0x28 (AOSP reserved[0])
 *   config/request       = AOSP layout
 * Print + _Static_assert everything.
 */
#include <stdint.h>
#include <stddef.h>

#include <hardware/camera3.h>

typedef struct samsung_camera3_stream {
    /* ---- AOSP base (identical field order to camera3.h in this tree) ---- */
    int stream_type;                    /* 0x00 */
    uint32_t width;                     /* 0x04 */
    uint32_t height;                    /* 0x08 */
    int format;                         /* 0x0c */
    uint32_t usage;                     /* 0x10 */
    uint32_t max_buffers;               /* 0x14 */
    void *priv;                         /* 0x18 */
    int32_t data_space;                 /* 0x1c */
    int rotation;                       /* 0x20 */
    const char *physical_camera_id;     /* 0x24 */
    int32_t streamOption;               /* 0x28  <- Samsung (AOSP reserved[0]) */
    void *reserved[5];                  /* 0x2c..0x3f */
    /* ---- Samsung extension 0x40..0x67 (zeroed by convertFromHidl) ---- */
    uint32_t samsung_ext[10];           /* 0x40..0x67 */
} samsung_camera3_stream_t;

typedef struct camera3_stream_ext_id {
    samsung_camera3_stream_t base;      /* 0x00..0x67 */
    int mId;                            /* 0x68 — Camera3Stream::mId */
} camera3_stream_ext_id_t;

#define OFF(s, f)  ((unsigned)offsetof(s, f))
#define SZ(s)      ((unsigned)sizeof(s))

/* Binary-derived ground truth (32-bit vendor stack) — compile-time verification */
_Static_assert(sizeof(camera3_stream_t) == 0x40, "AOSP 32-bit stream must be 0x40");
_Static_assert(offsetof(camera3_stream_t, usage) == 0x10, "usage @ 0x10");
_Static_assert(offsetof(camera3_stream_t, data_space) == 0x1c, "data_space @ 0x1c");
_Static_assert(offsetof(camera3_stream_t, rotation) == 0x20, "rotation @ 0x20");
_Static_assert(offsetof(camera3_stream_t, physical_camera_id) == 0x24, "physical_camera_id @ 0x24");
_Static_assert(offsetof(samsung_camera3_stream_t, streamOption) == 0x28, "Samsung streamOption @ 0x28 = AOSP reserved[0]");
_Static_assert(sizeof(samsung_camera3_stream_t) == 0x68, "Samsung camera3_stream_t = 0x68");
_Static_assert(offsetof(camera3_stream_ext_id_t, mId) == 0x68, "Camera3Stream::mId @ 0x68");
_Static_assert(sizeof(camera3_stream_configuration_t) == 0x10, "config = 0x10");
_Static_assert(offsetof(camera3_capture_request_t, num_output_buffers) == 0xc, "num_output_buffers @ 0xc");
_Static_assert(offsetof(camera3_capture_request_t, output_buffers) == 0x10, "output_buffers @ 0x10");
