/* rerinku-luckfox — camera program for Luckfox Pico (RV1103 / RV1106).
 *
 * Replaces Rockchip's reference application rkipc: brings up the sensor, ISP
 * and hardware encoder and connects the camera to a rerinku platform through
 * the rerinku device SDK. The encoder runs all the time (its frames are
 * dropped cheaply while nobody watches), so a viewer gets a picture as soon
 * as the next IDR, which the SDK requests when a session opens.
 *
 *   rerinku-luckfox [-c /userdata/rerinku/rerinku.conf] [--syslog] [key=value ...]
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "rerinku/rerinku_device.h"

#include "camera.h"
#include "config.h"
#include "log.h"
#include "sysinfo.h"

#define LOG_TAG "app"

#define DEFAULT_CONFIG "/userdata/rerinku/rerinku.conf"
#define VIDEO_CHANNEL 0u
#define SNAPSHOT_TIMEOUT_MS 2000
#define MAX_PENDING_SNAPSHOTS 8

static app_config g_cfg;
static rerinku_device_t *g_dev;
static char g_model[64];
static char g_serial[32]; /* CPU serial: the hardware identity */
static volatile sig_atomic_t g_reboot;

/* ---- snapshots -------------------------------------------------------------
 * on_snapshot only queues the request; one worker encodes a JPEG and answers
 * every request that arrived meanwhile with it. */

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char *pending[MAX_PENDING_SNAPSHOTS];
    int count;
    int quit;
    pthread_t thread;
    int started;
} snap = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

typedef struct {
    char **ids;
    int count;
} snap_batch;

static void reply_jpeg(void *user, const uint8_t *jpeg, size_t len) {
    snap_batch *b = user;
    for (int i = 0; i < b->count; i++)
        rerinku_device_reply_snapshot(g_dev, b->ids[i], jpeg, len); /* copies */
    LOGD("snapshot %zu bytes -> %d request(s)", len, b->count);
}

static void *snapshot_loop(void *arg) {
    prctl(PR_SET_NAME, "snapshot", 0, 0, 0);
    char *ids[MAX_PENDING_SNAPSHOTS];
    for (;;) {
        pthread_mutex_lock(&snap.mu);
        while (!snap.count && !snap.quit)
            pthread_cond_wait(&snap.cv, &snap.mu);
        if (snap.quit) {
            pthread_mutex_unlock(&snap.mu);
            break;
        }
        int n = snap.count;
        memcpy(ids, snap.pending, n * sizeof ids[0]);
        snap.count = 0;
        pthread_mutex_unlock(&snap.mu);

        snap_batch b = {ids, n};
        if (cam_snapshot(reply_jpeg, &b, SNAPSHOT_TIMEOUT_MS) != 0)
            for (int i = 0; i < n; i++)
                rerinku_device_reply_snapshot(g_dev, ids[i], NULL, 0); /* fails it */
        for (int i = 0; i < n; i++)
            free(ids[i]);
    }
    /* Requests still queued at shutdown die with the device. */
    pthread_mutex_lock(&snap.mu);
    for (int i = 0; i < snap.count; i++)
        free(snap.pending[i]);
    snap.count = 0;
    pthread_mutex_unlock(&snap.mu);
    return NULL;
}

/* ---- SDK callbacks ------------------------------------------------------------ */

static const char *state_name(rerinku_stream_state_t s) {
    static const char *names[] = {"new",    "connecting", "connected",
                                  "failed", "closed",     "disconnected"};
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}

static void on_status(void *user, rerinku_device_status_t status) {
    LOGI("platform: %s", rerinku_device_status_name(status));
}

static void on_stream_state(void *user, rerinku_stream_state_t state) {
    LOGI("stream: %s", state_name(state));
}

static int on_stream_start(void *user) {
    LOGI("stream requested");
    cam_request_idr();
    return 0;
}

static void on_stream_stop(void *user, const char *reason) {
    LOGI("stream stopped (%s)", reason ? reason : "");
}

static void on_keyframe_request(void *user, uint32_t channel_id) {
    cam_request_idr();
}

static void on_snapshot(void *user, const char *request_id) {
    pthread_mutex_lock(&snap.mu);
    char *id = snap.count < MAX_PENDING_SNAPSHOTS ? strdup(request_id) : NULL;
    if (id) {
        snap.pending[snap.count++] = id;
        pthread_cond_signal(&snap.cv);
    }
    pthread_mutex_unlock(&snap.mu);
    if (!id)
        rerinku_device_reply_snapshot(g_dev, request_id, NULL, 0);
}

static void on_command(void *user, const char *command_id, const char *action,
                       const char *params_json) {
    if (strcmp(action, "reboot") == 0) {
        rerinku_device_reply_command(g_dev, command_id, "{\"rebooting\":true}");
        LOGI("reboot requested by the platform");
        g_reboot = 1;
        kill(getpid(), SIGUSR1); /* wake the main thread */
        return;
    }
    rerinku_device_fail_command(g_dev, command_id, "unsupported command");
}

/* get_runtime: what the device looks like right now. The platform asks every
 * few seconds while someone has the device open, on the SDK's command thread.
 * Standard keys (tempC, cpuPct, memTotalKB, memAvailableKB, uptimeSec, load1)
 * get their own display; the rest is listed by name. */
static size_t on_runtime_info(void *user, char *buf, size_t cap) {
    static cam_stats prev;
    static double prev_at;
    sysinfo_stats s;
    cam_stats cs;
    sysinfo_read(&s);
    cam_get_stats(&cs);
    double cpu = sysinfo_cpu_percent();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;

    int n = snprintf(buf, cap,
                     "{\"uptimeSec\":%ld,\"load1\":%.2f,\"memTotalKB\":%ld,\"memAvailableKB\":%ld,"
                     "\"mediaPoolFreeKB\":%ld",
                     s.uptime_sec, s.load1, s.mem_total_kb, s.mem_available_kb, s.cma_free_kb);
    if (n > 0 && (size_t)n < cap && s.soc_temp_mc != INT32_MIN)
        n += snprintf(buf + n, cap - n, ",\"tempC\":%.1f", s.soc_temp_mc / 1000.0);
    if (n > 0 && (size_t)n < cap && cpu >= 0)
        n += snprintf(buf + n, cap - n, ",\"cpuPct\":%.1f", cpu);
    /* Encoder output since the last answer (skipped on the first). */
    double dt = now - prev_at;
    if (n > 0 && (size_t)n < cap && prev_at > 0 && dt > 0.5 && cs.frames >= prev.frames)
        n += snprintf(buf + n, cap - n, ",\"videoFps\":%.1f,\"videoKbps\":%.0f",
                      (double)(cs.frames - prev.frames) / dt,
                      (double)(cs.bytes - prev.bytes) * 8 / 1000 / dt);
    prev = cs;
    prev_at = now;
    if (n > 0 && (size_t)n + 2 <= cap) {
        buf[n++] = '}';
        buf[n] = '\0';
        return (size_t)n;
    }
    return 0;
}

/* Encoder thread. */
static void on_frame(void *user, const uint8_t *data, size_t len, uint64_t pts_us,
                     bool keyframe) {
    rerinku_write_result_t r = rerinku_device_write(g_dev, VIDEO_CHANNEL, data, len, pts_us);
    if (r == RERINKU_WRITE_RESULT_INVALID_FRAME)
        LOGW("encoder produced an invalid access unit (%zu bytes)", len);
}

/* ---- main -------------------------------------------------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-c config] [--syslog] [key=value ...]\n"
            "  -c FILE     settings file (default " DEFAULT_CONFIG ")\n"
            "  --syslog    log to syslog instead of stderr\n"
            "  key=value   override a setting, e.g. endpoint=http://192.168.1.10:18070\n"
            "  -V          print the version\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *config_path = DEFAULT_CONFIG;
    int use_syslog = 0, explicit_config = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
            explicit_config = 1;
        } else if (strcmp(argv[i], "--syslog") == 0) {
            use_syslog = 1;
        } else if (strcmp(argv[i], "-V") == 0) {
            printf("rerinku-luckfox %s (rerinku SDK %s)\n", APP_VERSION, rerinku_version());
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    config_defaults(&g_cfg);
    log_init(use_syslog, RERINKU_LOG_LEVEL_INFO);
    int rc = config_load(&g_cfg, config_path);
    if (rc == -1 || (rc == -2 && explicit_config)) {
        if (rc == -2)
            LOGE("cannot read %s: %s", config_path, strerror(errno));
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            i++;
            continue;
        }
        if (argv[i][0] == '-')
            continue;
        char err[160];
        if (config_apply(&g_cfg, argv[i], err, sizeof err) != 0) {
            LOGE("%s", err);
            return 2;
        }
    }
    log_init(use_syslog, (rerinku_log_level_t)g_cfg.log_level);
    if (config_finish(&g_cfg) != 0)
        return 2;
    sysinfo_model(g_model, sizeof g_model);
    if (sysinfo_cpu_serial(g_serial, sizeof g_serial) != 0)
        g_serial[0] = '\0';
    LOGI("rerinku-luckfox %s, SDK %s, %s, sn %s -> %s", APP_VERSION, rerinku_version(),
         g_model[0] ? g_model : "unknown board", g_cfg.sn, g_cfg.endpoint);

    /* Signals go to the main thread only: block them before any thread exists
     * (threads inherit the mask) and collect them with sigwait below. */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    signal(SIGPIPE, SIG_IGN);

    rerinku_device_config_t dc;
    rerinku_device_config_init(&dc);
    dc.endpoint = g_cfg.endpoint;
    dc.sn = g_cfg.sn;
    dc.name = g_cfg.name[0] ? g_cfg.name : NULL;
    dc.secret = g_cfg.secret[0] ? g_cfg.secret : NULL;
    dc.device_id = g_serial[0] ? g_serial : NULL;
    dc.model = g_model[0] ? g_model : "Luckfox Pico";
    dc.fw_version = "rerinku-luckfox " APP_VERSION;
    /* The static description the platform stores and shows: SoC, CPU,
     * memory, sensor, OS, MAC, plus a few facts of this board's own. */
    char sensor[32] = "", extra[160], profile[768];
    if (cam_probe_sensor_model(sensor, sizeof sensor) != 0)
        LOGW("could not read the sensor model");
    sysinfo_stats boot;
    sysinfo_read(&boot);
    snprintf(extra, sizeof extra, "\"manufacturer\":\"Luckfox\",\"mediaPoolKB\":%ld,\"sdk\":\"%s\"",
             boot.cma_total_kb, rerinku_version());
    if (sysinfo_profile_json(profile, sizeof profile, sensor, extra) > 0)
        dc.ext_json = profile;
    dc.ca_file = g_cfg.ca_file[0] ? g_cfg.ca_file : NULL;
    dc.tls_insecure = g_cfg.tls_insecure;
    /* No GOP cache: it would hold up to a GOP of 3 MP video in RAM. A new
     * viewer gets an IDR from the encoder instead (on_keyframe_request). */
    dc.gop_cache_max_bytes = 0;
    dc.nack_history_packets = (uint32_t)g_cfg.nack_history;

    rerinku_device_callbacks_t cb;
    memset(&cb, 0, sizeof cb);
    cb.struct_size = sizeof cb;
    cb.on_status = on_status;
    cb.on_stream_state = on_stream_state;
    cb.on_stream_start = on_stream_start;
    cb.on_stream_stop = on_stream_stop;
    cb.on_keyframe_request = on_keyframe_request;
    cb.on_command = on_command;
    cb.on_runtime_info = on_runtime_info;
    if (g_cfg.jpeg_quality > 0)
        cb.on_snapshot = on_snapshot;

    int err = rerinku_device_create(&dc, &cb, &g_dev);
    if (err != RERINKU_OK) {
        LOGE("rerinku_device_create: %s", rerinku_strerror(err));
        return 1;
    }
    bool h265 = strcmp(g_cfg.codec, "h265") == 0;
    rerinku_device_channel_t ch;
    memset(&ch, 0, sizeof ch);
    ch.struct_size = sizeof ch;
    ch.channel_id = VIDEO_CHANNEL;
    ch.kind = RERINKU_MEDIA_KIND_VIDEO;
    ch.codec = h265 ? RERINKU_CODEC_H265 : RERINKU_CODEC_H264;
    ch.bitstream_format = RERINKU_BITSTREAM_FORMAT_ANNEX_B; /* what VENC emits */
    ch.label = "main";
    ch.stream_id = "main";
    ch.width = (uint32_t)g_cfg.width;
    ch.height = (uint32_t)g_cfg.height;
    ch.fps = (uint32_t)g_cfg.fps;
    ch.bitrate_bps = (uint32_t)g_cfg.bitrate_kbps * 1000u;
    rerinku_device_add_channel(g_dev, &ch);

    /* Camera before the platform connection: a device that announces itself
     * should be able to stream. */
    cam_config cc = {
        .iq_dir = g_cfg.iq_dir,
        .h265 = h265,
        .width = g_cfg.width,
        .height = g_cfg.height,
        .fps = g_cfg.fps,
        .bitrate_kbps = g_cfg.bitrate_kbps,
        .gop = g_cfg.gop,
        .wrap = g_cfg.wrap != 0,
        .jpeg_quality = g_cfg.jpeg_quality,
    };
    if (cam_open(&cc, on_frame, NULL) != 0) {
        LOGE("camera start failed");
        rerinku_device_destroy(g_dev);
        return 1;
    }
    if (g_cfg.jpeg_quality > 0) {
        if (pthread_create(&snap.thread, NULL, snapshot_loop, NULL) == 0)
            snap.started = 1;
        else
            LOGW("snapshot thread could not start");
    }
    if ((err = rerinku_device_start(g_dev)) != RERINKU_OK) {
        LOGE("rerinku_device_start: %s", rerinku_strerror(err));
        rc = 1;
        goto out;
    }

    for (;;) {
        int sig = 0;
        if (sigwait(&sigs, &sig) != 0)
            continue;
        if (sig == SIGUSR1 && !g_reboot)
            continue;
        LOGI("%s", sig == SIGUSR1 ? "rebooting" : "shutting down");
        break;
    }
    rc = 0;

out:
    /* SDK first: it stops asking for frames, snapshots and IDRs, and tells
     * the platform the device is going offline. */
    rerinku_device_stop(g_dev);
    if (snap.started) {
        pthread_mutex_lock(&snap.mu);
        snap.quit = 1;
        pthread_cond_signal(&snap.cv);
        pthread_mutex_unlock(&snap.mu);
        pthread_join(snap.thread, NULL);
    }
    cam_close();
    rerinku_device_destroy(g_dev);
    if (g_reboot) {
        sync();
        execl("/sbin/reboot", "reboot", (char *)NULL);
        LOGE("reboot: %s", strerror(errno));
        return 1;
    }
    return rc;
}
