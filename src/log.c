#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void log_write(const char *level, const char *fmt, va_list ap)
{
    time_t now;
    struct tm tm_now;
    char stamp[20];

    now = time(NULL);
    tm_now = *localtime(&now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(stderr, "%s %s ", stamp, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("INFO", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("ERROR", fmt, ap);
    va_end(ap);
}
