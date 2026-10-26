/* The camera pipeline on RV1103/RV1106, through Rockchip's media stack:
 *
 *   sensor -> ISP (rkaiq 3A, IQ files) -> VI channel 0 -> VENC 0 (main, H.264/H.265)
 *                                      |                -> VENC 1 (JPEG, on demand)
 *                                      -> VI channel 1 -> VENC 2 (sub, optional)
 *
 * The sub stream comes from the ISP's second, downscaling output path, so it
 * costs a small encoder and a few small frame buffers, not a second capture.
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

    /* Sub stream: same frame rate and GOP as the main stream. */
    bool sub;
    bool sub_h265;
    int sub_width, sub_height;
    int sub_bitrate_kbps;
} cam_config;

enum { CAM_STREAM_MAIN = 0, CAM_STREAM_SUB = 1 };

/* Called on a stream's encoder thread for every encoded picture (one
 * complete access unit, Annex-B). stream: CAM_STREAM_MAIN or CAM_STREAM_SUB.
 * data is valid only during the call. pts_us: capture time in microseconds
 * (monotonic). */
typedef void (*cam_frame_fn)(void *user, int stream, const uint8_t *data, size_t len,
                             uint64_t pts_us, bool keyframe);

/* Starts ISP, VI and the encoders, then the encoder thread. 0 on success; on
 * failure everything started is torn down again. */
int cam_open(const cam_config *cfg, cam_frame_fn on_frame, void *user);
void cam_close(void);

/* The stream's next picture is an IDR. Any thread. */
void cam_request_idr(int stream);

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
    uint64_t frames, bytes, keyframes; /* main stream */
    uint32_t get_errors;
    uint64_t sub_frames, sub_bytes;
} cam_stats;
void cam_get_stats(cam_stats *s);

#endif
