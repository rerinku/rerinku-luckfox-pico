/* Settings, from a key = value file plus key=value command-line overrides.
 * See board/rerinku.conf.example for the keys. */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

typedef struct app_config {
    /* platform */
    char endpoint[256]; /* onboard base URL, e.g. http://192.168.1.10:18070 */
    char sn[64];        /* empty: LUCKFOX-<CPU serial> */
    char name[64];      /* empty: the platform shows the SN */
    char secret[128];   /* base64(HMAC-SHA256(device key, sn)) when keys are enforced */
    char ca_file[256];
    int tls_insecure;

    /* camera */
    char iq_dir[128];
    char codec[8]; /* h264 | h265 */
    int width, height, fps;
    int bitrate_kbps;
    int gop;          /* frames between keyframes */
    int wrap;         /* VI->VENC line-buffer (wrap) mode: saves ~4 MB of CMA */
    int jpeg_quality; /* snapshot JPEG quality 1-99, 0 = no snapshots */

    /* SDK */
    int nack_history; /* packets kept for retransmission */
    int log_level;    /* rerinku_log_level_t */
} app_config;

void config_defaults(app_config *c);
/* Applies one "key = value" (or "key=value") line; '#' starts a comment.
 * Returns 0, or -1 with a message in err. */
int config_apply(app_config *c, const char *line, char *err, int errlen);
/* Reads a file of such lines. Returns 0, -1 on a bad line (logged), -2 when
 * the file can't be opened. */
int config_load(app_config *c, const char *path);
/* Checks ranges and combinations; fills defaults that depend on the board. */
int config_finish(app_config *c);

#endif
