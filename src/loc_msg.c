#include "loc_msg.h"

#include <string.h>

static void put_u32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)((v >> 24) & 0xFFu);
    out[1] = (uint8_t)((v >> 16) & 0xFFu);
    out[2] = (uint8_t)((v >> 8) & 0xFFu);
    out[3] = (uint8_t)(v & 0xFFu);
}

static void put_u64(uint8_t *out, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        out[7 - i] = (uint8_t)((v >> (i * 8)) & 0xFFu);
    }
}

/* NMEA 的 ddmm.mmmm / dddmm.mmmm 换成 1e-7 度。分钟是小数点前两位。 */
static int nmea_to_e7(const char *field, int *out)
{
    const char *dot;
    const char *min_at;
    const char *p;
    long long deg;
    long long minu;
    long long scale;

    if (field == NULL || field[0] == '\0') {
        return -1;
    }
    dot = strchr(field, '.');
    if (dot == NULL || dot < field + 2) {
        return -1;
    }
    min_at = dot - 2;
    deg = 0;
    for (p = field; p < min_at; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        deg = deg * 10 + (*p - '0');
    }
    minu = 0;
    scale = 1;
    for (p = min_at; p < dot; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        minu = minu * 10 + (*p - '0');
    }
    for (p = dot + 1; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        if (scale > 1000000LL) {
            break;
        }
        minu = minu * 10 + (*p - '0');
        scale *= 10;
    }
    *out = (int)(deg * 10000000LL + (minu * 10000000LL) / (60 * scale));
    return 0;
}

/* RMC 时间取小数点前 6 位，083015 表示 08:30:15。 */
static int utc_hhmmss(const char *text, uint32_t *out)
{
    uint32_t v;
    int n;

    if (text == NULL) {
        return -1;
    }
    v = 0;
    n = 0;
    while (text[n] != '\0' && text[n] != '.') {
        if (text[n] < '0' || text[n] > '9' || n >= 6) {
            return -1;
        }
        v = v * 10u + (uint32_t)(text[n] - '0');
        n++;
    }
    if (n != 6) {
        return -1;
    }
    *out = v;
    return 0;
}

int loc_build(uint8_t *out, int cap, uint64_t seq_ms, const struct gps_fix *fix)
{
    int lat;
    int lon;
    uint32_t utc;
    int valid;

    if (out == NULL || cap < 22) {
        return -1;
    }
    memset(out, 0, 22);
    out[0] = LOC_CMD;
    put_u64(out + 1, seq_ms);

    valid = 0;
    lat = 0;
    lon = 0;
    utc = 0;
    if (fix != NULL && fix->valid) {
        if (nmea_to_e7(fix->lat, &lat) == 0 && nmea_to_e7(fix->lon, &lon) == 0 &&
            utc_hhmmss(fix->utc, &utc) == 0) {
            if (fix->ns == 'S') {
                lat = -lat;
            }
            if (fix->ew == 'W') {
                lon = -lon;
            }
            if ((fix->ns == 'N' || fix->ns == 'S') &&
                (fix->ew == 'E' || fix->ew == 'W')) {
                valid = 1;
            }
        }
    }
    out[9] = (uint8_t)valid;
    if (valid) {
        put_u32(out + 10, (uint32_t)lat);
        put_u32(out + 14, (uint32_t)lon);
        put_u32(out + 18, utc);
    }
    return 22;
}
