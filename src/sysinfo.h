/* Board facts: the device's static description (ext_json) and its live
 * runtime state (get_runtime). */
#ifndef APP_SYSINFO_H
#define APP_SYSINFO_H

#include <stddef.h>

/* CPU serial from /proc/cpuinfo, upper-case hex. 0 on success. */
int sysinfo_cpu_serial(char *buf, size_t len);
/* Device-tree model ("Luckfox Pico Plus"), or "" when unknown. */
void sysinfo_model(char *buf, size_t len);

typedef struct sysinfo_stats {
    long uptime_sec;
    long mem_total_kb, mem_available_kb;
    long cma_total_kb, cma_free_kb; /* the media buffer pool */
    int soc_temp_mc;                /* milli-degrees C, INT32_MIN when unknown */
    double load1;
} sysinfo_stats;

void sysinfo_read(sysinfo_stats *s);

/* Whole-system CPU busy percentage since the previous call (the first call
 * measures over a short window, briefly blocking). -1 when unknown. Not
 * thread-safe: call from one thread. */
double sysinfo_cpu_percent(void);

/* The static description as a JSON object (SoC, CPU, memory, OS, MAC...),
 * with the given sensor model and extra pre-rendered members appended
 * (`"key":value,...`, may be NULL). Returns the length, 0 on overflow. */
size_t sysinfo_profile_json(char *buf, size_t cap, const char *sensor, const char *extra);

#endif
