#ifndef GPS_FIX_H
#define GPS_FIX_H

struct gps_fix {
    int valid;
    char utc[16];
    char lat[16];
    char ns;
    char lon[16];
    char ew;
};

/* 读一段时间 NMEA。打开失败返回 -1。没有有效定位时 valid 为 0，仍返回 0。 */
int gps_read_once(const char *path, unsigned baud, struct gps_fix *fix, int timeout_ms);

#endif
