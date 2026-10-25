/* Logging for the camera program and the SDK: stderr with timestamps when run
 * by hand, syslog when started at boot. */
#ifndef APP_LOG_H
#define APP_LOG_H

#include "rerinku/rerinku_device.h"

void log_init(int use_syslog, rerinku_log_level_t level);
/* "error" / "warn" / "info" / "debug"; -1 when unknown. */
int log_level_parse(const char *name);

void log_write(rerinku_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOGE(...) log_write(RERINKU_LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) log_write(RERINKU_LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)
#define LOGI(...) log_write(RERINKU_LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) log_write(RERINKU_LOG_LEVEL_DEBUG, LOG_TAG, __VA_ARGS__)

#endif
