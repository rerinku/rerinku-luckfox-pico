#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "sysinfo.h"

#define LOG_TAG "config"

enum kind { STR, INT, LEVEL };

typedef struct {
    const char *key;
    enum kind kind;
    size_t offset;
    size_t size; /* STR: buffer size */
    int min, max;
} field;

#define S(k, m) {k, STR, offsetof(app_config, m), sizeof(((app_config *)0)->m), 0, 0}
#define I(k, m, lo, hi) {k, INT, offsetof(app_config, m), 0, lo, hi}

static const field fields[] = {
    S("endpoint", endpoint),
    S("sn", sn),
    S("name", name),
    S("secret", secret),
    S("ca_file", ca_file),
    I("tls_insecure", tls_insecure, 0, 1),
    S("iq_dir", iq_dir),
    S("codec", codec),
    I("width", width, 64, 4096),
    I("height", height, 64, 4096),
    I("fps", fps, 1, 60),
    I("bitrate", bitrate_kbps, 64, 20000),
    I("gop", gop, 1, 600),
    I("wrap", wrap, 0, 1),
    I("jpeg_quality", jpeg_quality, 0, 99),
    I("sub", sub, 0, 1),
    S("sub_codec", sub_codec),
    I("sub_width", sub_width, 64, 1920),
    I("sub_height", sub_height, 64, 1080),
    I("sub_bitrate", sub_bitrate_kbps, 32, 8000),
    I("nack_history", nack_history, 0, 4096),
    {"log_level", LEVEL, offsetof(app_config, log_level), 0, 0, 0},
};

void config_defaults(app_config *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->iq_dir, sizeof c->iq_dir, "/oem/usr/share/iqfiles");
    /* H.265 main stream with an H.264 sub stream: the platform plays the sub
     * stream to browsers that cannot decode H.265. */
    snprintf(c->codec, sizeof c->codec, "h265");
    /* SC3336 full frame. */
    c->width = 2304;
    c->height = 1296;
    c->fps = 25;
    c->bitrate_kbps = 2048;
    c->gop = 50;
    c->wrap = 1;
    c->jpeg_quality = 70;
    c->sub = 1;
    snprintf(c->sub_codec, sizeof c->sub_codec, "h264");
    /* 16:9 like the main stream; small enough for a multi-camera wall. */
    c->sub_width = 640;
    c->sub_height = 360;
    c->sub_bitrate_kbps = 512;
    c->nack_history = 256;
    c->log_level = RERINKU_LOG_LEVEL_INFO;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

int config_apply(app_config *c, const char *line, char *err, int errlen) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", line);
    char *hash = strchr(buf, '#');
    if (hash)
        *hash = '\0';
    char *s = trim(buf);
    if (!*s)
        return 0;
    char *eq = strchr(s, '=');
    if (!eq) {
        snprintf(err, errlen, "expected key = value: %s", s);
        return -1;
    }
    *eq = '\0';
    char *key = trim(s), *val = trim(eq + 1);
    /* Optional quotes around a value. */
    size_t n = strlen(val);
    if (n >= 2 && (val[0] == '"' || val[0] == '\'') && val[n - 1] == val[0]) {
        val[n - 1] = '\0';
        val++;
    }

    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const field *f = &fields[i];
        if (strcmp(f->key, key) != 0)
            continue;
        char *dst = (char *)c + f->offset;
        if (f->kind == STR) {
            if (strlen(val) >= f->size) {
                snprintf(err, errlen, "%s: value too long", key);
                return -1;
            }
            memcpy(dst, val, strlen(val) + 1);
        } else if (f->kind == LEVEL) {
            int lv = log_level_parse(val);
            if (lv < 0) {
                snprintf(err, errlen, "log_level: error, warn, info or debug");
                return -1;
            }
            *(int *)dst = lv;
        } else {
            char *end;
            errno = 0;
            long v = strtol(val, &end, 10);
            if (errno || end == val || *end || v < f->min || v > f->max) {
                snprintf(err, errlen, "%s: integer %d..%d expected, got \"%s\"", key, f->min,
                         f->max, val);
                return -1;
            }
            *(int *)dst = (int)v;
        }
        return 0;
    }
    snprintf(err, errlen, "unknown key \"%s\"", key);
    return -1;
}

int config_load(app_config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return -2;
    char line[512], err[160];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (config_apply(c, line, err, sizeof err) != 0) {
            LOGE("%s:%d: %s", path, lineno, err);
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}

int config_finish(app_config *c) {
    if (!c->endpoint[0]) {
        LOGE("endpoint is not set (the rerinku platform URL, e.g. http://192.168.1.10:18070)");
        return -1;
    }
    if (strcmp(c->codec, "h264") != 0 && strcmp(c->codec, "h265") != 0) {
        LOGE("codec must be h264 or h265");
        return -1;
    }
    /* The encoder works on 16x16 blocks of 4:2:0 frames; odd sizes fail. */
    if (c->width % 2 || c->height % 2) {
        LOGE("width and height must be even");
        return -1;
    }
    if (strcmp(c->sub_codec, "h264") != 0 && strcmp(c->sub_codec, "h265") != 0) {
        LOGE("sub_codec must be h264 or h265");
        return -1;
    }
    if (c->sub && (c->sub_width % 2 || c->sub_height % 2 || c->sub_width > c->width ||
                   c->sub_height > c->height)) {
        LOGE("sub_width/sub_height must be even and no larger than the main stream");
        return -1;
    }
    if (!c->sn[0]) {
        char serial[32];
        if (sysinfo_cpu_serial(serial, sizeof serial) != 0) {
            LOGE("sn is not set and the CPU serial is unavailable");
            return -1;
        }
        snprintf(c->sn, sizeof c->sn, "LUCKFOX-%s", serial);
    }
    return 0;
}
