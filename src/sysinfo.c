#include "sysinfo.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

int sysinfo_cpu_serial(char *buf, size_t len) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return -1;
    char line[256];
    int rc = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Serial", 6) != 0)
            continue;
        char *v = strchr(line, ':');
        if (!v)
            break;
        v++;
        while (*v == ' ' || *v == '\t')
            v++;
        size_t n = 0;
        for (; v[n] && isxdigit((unsigned char)v[n]) && n + 1 < len; n++)
            buf[n] = (char)toupper((unsigned char)v[n]);
        buf[n] = '\0';
        /* All zeros means the fuse was never written. */
        if (n > 0 && strspn(buf, "0") != n)
            rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

void sysinfo_model(char *buf, size_t len) {
    buf[0] = '\0';
    FILE *f = fopen("/proc/device-tree/model", "r");
    if (!f)
        return;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* The property is NUL-terminated and may have trailing blanks. */
    n = strlen(buf);
    while (n > 0 && isspace((unsigned char)buf[n - 1]))
        buf[--n] = '\0';
}

static long meminfo_kb(const char *text, const char *key) {
    const char *p = strstr(text, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

void sysinfo_read(sysinfo_stats *s) {
    memset(s, 0, sizeof *s);
    s->soc_temp_mc = INT32_MIN;
    char text[2048];
    FILE *f;

    if ((f = fopen("/proc/uptime", "r"))) {
        double up;
        if (fscanf(f, "%lf", &up) == 1)
            s->uptime_sec = (long)up;
        fclose(f);
    }
    if ((f = fopen("/proc/loadavg", "r"))) {
        if (fscanf(f, "%lf", &s->load1) != 1)
            s->load1 = 0;
        fclose(f);
    }
    if ((f = fopen("/proc/meminfo", "r"))) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = '\0';
        fclose(f);
        s->mem_total_kb = meminfo_kb(text, "MemTotal:");
        s->mem_available_kb = meminfo_kb(text, "MemAvailable:");
    }
    /* The media buffers come from a CMA region reserved by rk_dma_heap;
     * /proc/meminfo reports it as permanently allocated. */
    if ((f = fopen("/proc/rk_dma_heap/alloc_bitmap", "r"))) {
        size_t n = fread(text, 1, 256, f);
        text[n] = '\0';
        fclose(f);
        long total = meminfo_kb(text, "Total:"), used = meminfo_kb(text, "Used:");
        if (total > 0 && used >= 0) {
            s->cma_total_kb = total;
            s->cma_free_kb = total - used;
        }
    }
    if ((f = fopen("/sys/class/thermal/thermal_zone0/temp", "r"))) {
        int t;
        if (fscanf(f, "%d", &t) == 1)
            s->soc_temp_mc = t;
        fclose(f);
    }
}

/* ---- CPU load ------------------------------------------------------------- */

static int read_cpu_times(unsigned long long *busy, unsigned long long *total) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                   &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
        return -1;
    unsigned long long idle = v[3] + v[4]; /* idle + iowait */
    *total = 0;
    for (int i = 0; i < 8; i++)
        *total += v[i];
    *busy = *total - idle;
    return 0;
}

static double monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double sysinfo_cpu_percent(void) {
    static unsigned long long prev_busy, prev_total;
    static double prev_at;
    unsigned long long busy, total;
    double now = monotonic_sec();
    /* No recent sample to compare with: take one now and wait a moment. */
    if (prev_at == 0 || now - prev_at > 60) {
        if (read_cpu_times(&prev_busy, &prev_total) != 0)
            return -1;
        struct timespec pause = {0, 250 * 1000 * 1000};
        nanosleep(&pause, NULL);
        now = monotonic_sec();
    }
    if (read_cpu_times(&busy, &total) != 0)
        return -1;
    double pct = -1;
    if (total > prev_total)
        pct = 100.0 * (double)(busy - prev_busy) / (double)(total - prev_total);
    prev_busy = busy;
    prev_total = total;
    prev_at = now;
    return pct;
}

/* ---- static description ---------------------------------------------------- */

static void read_first_line(const char *path, char *buf, size_t len) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(buf, (int)len, f))
        buf[strcspn(buf, "\r\n")] = '\0';
    fclose(f);
}

/* "rockchip,rv1103g-38x38-ipc-v10\0rockchip,rv1103\0": the last entry is the
 * SoC. "rockchip,rv1103" -> "Rockchip RV1103". */
static void read_soc(char *buf, size_t len) {
    buf[0] = '\0';
    char raw[256];
    FILE *f = fopen("/proc/device-tree/compatible", "r");
    if (!f)
        return;
    size_t n = fread(raw, 1, sizeof raw - 1, f);
    fclose(f);
    raw[n] = '\0';
    const char *last = NULL;
    for (size_t i = 0; i < n; i += strlen(raw + i) + 1)
        if (raw[i])
            last = raw + i;
    const char *comma = last ? strchr(last, ',') : NULL;
    if (!comma)
        return;
    size_t o = 0;
    for (const char *p = last; p < comma && o + 1 < len; p++)
        buf[o++] = (char)(p == last ? toupper((unsigned char)*p) : *p);
    if (o + 1 < len)
        buf[o++] = ' ';
    for (const char *p = comma + 1; *p && o + 1 < len; p++)
        buf[o++] = (char)toupper((unsigned char)*p);
    buf[o] = '\0';
}

/* Core name from the ARM part number; the generic "model name" otherwise. */
static void read_cpu(char *name, size_t len, int *cores) {
    static const struct { unsigned part; const char *name; } arm[] = {
        {0xc05, "Cortex-A5"},  {0xc07, "Cortex-A7"},  {0xc08, "Cortex-A8"},
        {0xc09, "Cortex-A9"},  {0xc0d, "Cortex-A12"}, {0xc0f, "Cortex-A15"},
        {0xc0e, "Cortex-A17"}, {0xd03, "Cortex-A53"}, {0xd04, "Cortex-A35"},
        {0xd05, "Cortex-A55"}, {0xd07, "Cortex-A57"}, {0xd08, "Cortex-A72"},
        {0xd0b, "Cortex-A76"},
    };
    name[0] = '\0';
    *cores = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return;
    char line[256], model[128] = "";
    unsigned part = 0;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, ':');
        if (!v)
            continue;
        v++;
        while (*v == ' ' || *v == '\t')
            v++;
        v[strcspn(v, "\r\n")] = '\0';
        if (strncmp(line, "processor", 9) == 0)
            (*cores)++;
        else if (strncmp(line, "CPU part", 8) == 0 && !part)
            part = (unsigned)strtoul(v, NULL, 0);
        else if (strncmp(line, "model name", 10) == 0 && !model[0])
            snprintf(model, sizeof model, "%s", v);
    }
    fclose(f);
    for (size_t i = 0; i < sizeof arm / sizeof arm[0]; i++)
        if (arm[i].part == part)
            snprintf(name, len, "%s", arm[i].name);
    if (!name[0])
        snprintf(name, len, "%.*s", (int)(len > 0 ? len - 1 : 0), model);
}

/* Appends "key":"value" with JSON escaping; returns the new length (cap on
 * overflow, which the caller detects). */
static size_t put_str(char *buf, size_t cap, size_t n, const char *key, const char *value) {
    if (!value || !value[0] || n >= cap)
        return n;
    n += (size_t)snprintf(buf + n, cap - n, "%s\"%s\":\"", n > 1 ? "," : "", key);
    for (const char *p = value; *p && n + 7 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')
            n += (size_t)snprintf(buf + n, cap - n, "\\%c", c);
        else if (c < 0x20)
            n += (size_t)snprintf(buf + n, cap - n, "\\u%04x", c);
        else
            buf[n++] = (char)c;
    }
    if (n + 1 < cap)
        buf[n++] = '"';
    else
        n = cap;
    if (n < cap)
        buf[n] = '\0';
    return n;
}

static size_t put_num(char *buf, size_t cap, size_t n, const char *key, long value) {
    if (value <= 0 || n >= cap)
        return n;
    n += (size_t)snprintf(buf + n, cap - n, "%s\"%s\":%ld", n > 1 ? "," : "", key, value);
    return n < cap ? n : cap;
}

size_t sysinfo_profile_json(char *buf, size_t cap, const char *sensor, const char *extra) {
    if (cap < 3)
        return 0;
    char soc[64], cpu[64], freq[32], os[96] = "", mac[32];
    int cores = 0;
    read_soc(soc, sizeof soc);
    read_cpu(cpu, sizeof cpu, &cores);
    read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", freq, sizeof freq);
    read_first_line("/sys/class/net/eth0/address", mac, sizeof mac);
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(os, sizeof os, "%.32s %.48s", u.sysname, u.release);
    sysinfo_stats s;
    sysinfo_read(&s);

    size_t n = 0;
    buf[n++] = '{';
    buf[n] = '\0';
    n = put_str(buf, cap, n, "soc", soc);
    n = put_str(buf, cap, n, "cpu", cpu);
    n = put_num(buf, cap, n, "cpuCores", cores);
    n = put_num(buf, cap, n, "cpuMHz", strtol(freq, NULL, 10) / 1000);
    n = put_num(buf, cap, n, "memoryKB", s.mem_total_kb);
    n = put_str(buf, cap, n, "sensor", sensor);
    n = put_str(buf, cap, n, "os", os);
    n = put_str(buf, cap, n, "mac", mac);
    if (extra && extra[0] && n < cap)
        n += (size_t)snprintf(buf + n, cap - n, "%s%s", n > 1 ? "," : "", extra);
    if (n + 2 > cap)
        return 0;
    buf[n++] = '}';
    buf[n] = '\0';
    return n;
}
