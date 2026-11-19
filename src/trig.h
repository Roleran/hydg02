#ifndef TRIG_H
#define TRIG_H

#include <stdint.h>

struct voice;
struct led;

#define TRIG_MODE    0x50u
#define TRIG_WINDOW  0x51u
#define TRIG_QUERY   0x52u
#define TRIG_MANUAL  0x53u
#define TRIG_STATE   0xA3u
#define TRIG_EVENT   0xA4u

#define TRIG_RADAR   0
#define TRIG_CAMERA  1

struct trig_cmd {
    uint8_t cmd;
    uint64_t seq;
    int mode;
    int timer_on;
    int start_h;
    int start_m;
    int end_h;
    int end_m;
};

struct trig {
    struct voice *voice;
    struct led *led;
    int mode;
    int timer_on;
    int start_h;
    int start_m;
    int end_h;
    int end_m;
    int have_seq;
    uint8_t last_seq;
    int pending;
    int queued;
    int acting;
    char dev[64];
    unsigned baud;
};

/* 定时关闭时允许。时段可跨过午夜，结束时刻不算在内。 */
int trig_allows(int timer_on, int start_min, int end_min, int now_min);

int trig_parse(const uint8_t *buf, int len, struct trig_cmd *cmd);
int trig_state(uint8_t *out, int cap, uint64_t seq, const struct trig *trig);

void trig_init(struct trig *trig, struct voice *voice, struct led *led,
               const char *dev, unsigned baud);

/* 屏口里看到 0x90 时马上回 0x8F。动作等到 trig_service。 */
void trig_seen_radar(int fd, uint8_t seq);

void trig_service(struct trig *trig,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  int (*pump)(void *ctx, int idle_ms), void *ctx);

int trig_on_packet(struct trig *trig, const uint8_t *buf, int len,
                   int (*publish)(void *ctx, const uint8_t *data, int n),
                   int (*pump)(void *ctx, int idle_ms), void *ctx);

#endif
