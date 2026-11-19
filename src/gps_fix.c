#include "gps_fix.h"

#include <string.h>

#ifndef _WIN32

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHDEV_IOC_SET_BAUD _IOW('C', 3, unsigned int)

static void copy_field(char *dst, int cap, const char *src)
{
    int i;

    for (i = 0; i < cap - 1 && src[i] != '\0' && src[i] != ','; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static const char *skip_fields(const char *p, int count)
{
    int i;

    for (i = 0; i < count && p != NULL; i++) {
        p = strchr(p, ',');
        if (p != NULL) {
            p++;
        }
    }
    return p;
}

static int checksum_ok(const char *line)
{
    const char *star;
    unsigned char sum;
    const char *p;
    unsigned int got;

    if (line[0] != '$') {
        return 0;
    }
    star = strchr(line, '*');
    if (star == NULL) {
        return 0;
    }
    sum = 0;
    for (p = line + 1; p < star; p++) {
        sum = (unsigned char)(sum ^ (unsigned char)*p);
    }
    if (sscanf(star + 1, "%2x", &got) != 1) {
        return 0;
    }
    return sum == (unsigned char)got;
}

/* $--RMC,时间,A/V,纬度,N/S,经度,E/W */
static int parse_rmc(const char *line, struct gps_fix *fix)
{
    const char *status;
    const char *lat;
    const char *ns;
    const char *lon;
    const char *ew;
    const char *utc;

    if (strstr(line, "RMC,") == NULL || !checksum_ok(line)) {
        return 0;
    }
    utc = strchr(line, ',');
    if (utc == NULL) {
        return 0;
    }
    utc++;
    status = skip_fields(utc, 1);
    lat = skip_fields(utc, 2);
    ns = skip_fields(utc, 3);
    lon = skip_fields(utc, 4);
    ew = skip_fields(utc, 5);
    if (status == NULL || status[0] != 'A' || lat == NULL || ns == NULL ||
        lon == NULL || ew == NULL) {
        return 0;
    }
    copy_field(fix->utc, (int)sizeof(fix->utc), utc);
    copy_field(fix->lat, (int)sizeof(fix->lat), lat);
    copy_field(fix->lon, (int)sizeof(fix->lon), lon);
    fix->ns = ns[0];
    fix->ew = ew[0];
    fix->valid = 1;
    return 1;
}

int gps_read_once(const char *path, unsigned baud, struct gps_fix *fix, int timeout_ms)
{
    int fd;
    int left;
    char line[128];
    int used;

    if (path == NULL || fix == NULL) {
        return -1;
    }
    memset(fix, 0, sizeof(*fix));
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (ioctl(fd, CHDEV_IOC_SET_BAUD, &baud) != 0) {
        close(fd);
        return -1;
    }

    used = 0;
    left = timeout_ms;
    while (left > 0 && !fix->valid) {
        struct pollfd pfd;
        char chunk[64];
        int n;
        int i;
        int wait = left > 500 ? 500 : left;

        pfd.fd = fd;
        pfd.events = POLLIN;
        n = poll(&pfd, 1, wait);
        left -= wait;
        if (n <= 0) {
            continue;
        }
        n = (int)read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            continue;
        }
        for (i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r' || c == '\n') {
                line[used] = '\0';
                if (used > 6) {
                    parse_rmc(line, fix);
                }
                used = 0;
                continue;
            }
            if (used < (int)sizeof(line) - 1) {
                line[used++] = c;
            } else {
                used = 0;
            }
        }
    }
    close(fd);
    return 0;
}

#else

int gps_read_once(const char *path, unsigned baud, struct gps_fix *fix, int timeout_ms)
{
    (void)path;
    (void)baud;
    (void)timeout_ms;
    if (fix != NULL) {
        memset(fix, 0, sizeof(*fix));
    }
    return -1;
}

#endif
