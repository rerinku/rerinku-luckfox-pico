#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>

static int g_syslog;
static rerinku_log_level_t g_level = RERINKU_LOG_INFO;

static void emit(rerinku_log_level_t level, const char *tag, const char *msg) {
    if (level > g_level)
        return;
    if (g_syslog) {
        static const int prio[] = {LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG};
        syslog(prio[level], "%s: %s", tag, msg);
        return;
    }
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    /* One fprintf per line: stdio locks the stream, so lines from SDK
     * threads don't interleave. */
    fprintf(stderr, "%02d:%02d:%02d.%03d %c %s: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
            (int)(tv.tv_usec / 1000), "EWID"[level], tag, msg);
}

static void sdk_log(void *user, rerinku_log_level_t level, const char *tag,
                    const char *message) {
    char t[32];
    snprintf(t, sizeof t, "sdk.%s", tag);
    emit(level, t, message);
}

void log_init(int use_syslog, rerinku_log_level_t level) {
    g_syslog = use_syslog;
    g_level = level;
    if (use_syslog)
        openlog("rerinku", LOG_PID, LOG_DAEMON);
    else
        setvbuf(stderr, NULL, _IOLBF, 0);
    rerinku_set_log_level(level);
    rerinku_set_log_handler(sdk_log, NULL);
}

int log_level_parse(const char *name) {
    static const char *names[] = {"error", "warn", "info", "debug"};
    for (int i = 0; i < 4; i++)
        if (strcmp(name, names[i]) == 0)
            return i;
    return -1;
}

void log_write(rerinku_log_level_t level, const char *tag, const char *fmt, ...) {
    if (level > g_level)
        return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    emit(level, tag, msg);
}
