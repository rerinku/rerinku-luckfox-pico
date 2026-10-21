/* The camera pipeline on RV1103/RV1106, through Rockchip's media stack:
 *
 *   sensor -> ISP (rkaiq 3A, IQ files) -> VI channel 0 -> VENC 0 (H.264/H.265)
 *                                                       -> VENC 1 (JPEG, on demand)
 *
 * VI and VENC are bound in hardware; the program only collects encoded
 * frames. In wrap mode VI hands the encoder a ring of lines instead of whole
 * frames, which is what makes a 3 MP stream fit in RV1103's memory. */
#ifndef APP_CAMERA_H
#define APP_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cam_config {
    const char *iq_dir; /* rkaiq IQ files (per-sensor tuning) */
    bool h265;
    int width, height, fps;
    int bitrate_kbps; /* CBR */
    int gop;          /* frames between IDRs */
    bool wrap;
    int jpeg_quality; /* 0: no JPEG channel */
} cam_config;

/* Called on the encoder thread for every encoded picture (one complete access
 * unit, Annex-B). data is valid only during the call. pts_us: capture time
 * in microseconds (monotonic). */
typedef void (*cam_frame_fn)(void *user, const uint8_t *data, size_t len, uint64_t pts_us,
                             bool keyframe);

/* Starts ISP, VI and the encoders, then the encoder thread. 0 on success; on
 * failure everything started is torn down again. */
int cam_open(const cam_config *cfg, cam_frame_fn on_frame, void *user);
void cam_close(void);

/* Next picture out of VENC 0 is an IDR. Any thread. */
void cam_request_idr(void);

/* Encodes the next frame as JPEG and passes it to fn (data valid only during
 * the call). Blocks up to timeout_ms. One caller at a time. 0 on success. */
int cam_snapshot(void (*fn)(void *user, const uint8_t *jpeg, size_t len), void *user,
                 int timeout_ms);

/* Sensor entity name reported by rkaiq ("m00_b_sc3336 4-0030"), "" before
 * cam_open. */
const char *cam_sensor_name(void);

/* Sensor model ("SC3336") from the camera drivers, before cam_open; for the
 * device description. 0 on success. */
int cam_probe_sensor_model(char *buf, size_t len);

typedef struct cam_stats {
    uint64_t frames, bytes, keyframes;
    uint32_t get_errors;
} cam_stats;
void cam_get_stats(cam_stats *s);

#endif
