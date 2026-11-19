#include "trig.h"

#include "admin.h"
#include "led.h"
#include "log.h"
#include "n32_frame.h"
#include "voice.h"

#include <stdio.h>
#include <string.h>

#define TRIG_FILE "/etc/hydg02/trig.bin"

static struct trig *current;

static uint64_t read_seq(const uint8_t *buf)
{
    uint64_t seq;
    int i;

    seq = 0;
    for (i = 0; i < 8; i++) {
        seq = (seq << 8) | buf[1 + i];
    }
    return seq;
}

int trig_allows(int timer_on, int start_min, int end_min, int now_min)
{
    if (!timer_on) {
        return 1;
    }
    if (now_min < 0 || now_min >= 24 * 60) {
        return 0;
    }
    if (start_min == end_min) {
        return 0;
    }
    if (start_min < end_min) {
        return now_min >= start_min && now_min < end_min;
    }
    return now_min >= start_min || now_min < end_min;
}

int trig_parse(const uint8_t *buf, int len, struct trig_cmd *cmd)
{
    if (buf == NULL || cmd == NULL || len < 9) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = buf[0];
    cmd->seq = read_seq(buf);
    if (cmd->cmd != TRIG_MODE && cmd->cmd != TRIG_WINDOW &&
        cmd->cmd != TRIG_QUERY && cmd->cmd != TRIG_MANUAL) {
        return 1;
    }
    if (cmd->cmd == TRIG_QUERY || cmd->cmd == TRIG_MANUAL) {
        return len == 9 ? 0 : -1;
    }
    if (cmd->cmd == TRIG_MODE) {
        if (len != 10 || (buf[9] != TRIG_RADAR && buf[9] != TRIG_CAMERA)) {
            return -1;
        }
        cmd->mode = buf[9];
        return 0;
    }
    if (len != 14) {
        return -1;
    }
    cmd->timer_on = buf[9];
    cmd->start_h = buf[10];
    cmd->start_m = buf[11];
    cmd->end_h = buf[12];
    cmd->end_m = buf[13];
    if ((cmd->timer_on != 0 && cmd->timer_on != 1) ||
        cmd->start_h > 23 || cmd->end_h > 23 || cmd->start_m > 59 || cmd->end_m > 59) {
        return -1;
    }
    return 0;
}

int trig_state(uint8_t *out, int cap, uint64_t seq, const struct trig *trig)
{
    int i;

    if (out == NULL || trig == NULL || cap < 15) {
        return -1;
    }
    out[0] = TRIG_STATE;
    for (i = 7; i >= 0; i--) {
        out[1 + (7 - i)] = (uint8_t)((seq >> (i * 8)) & 0xFFu);
    }
    out[9] = (uint8_t)trig->mode;
    out[10] = (uint8_t)(trig->timer_on ? 1 : 0);
    out[11] = (uint8_t)trig->start_h;
    out[12] = (uint8_t)trig->start_m;
    out[13] = (uint8_t)trig->end_h;
    out[14] = (uint8_t)trig->end_m;
    return 15;
}

static void trig_load(struct trig *trig)
{
    FILE *fp;
    uint8_t raw[6];

    trig->mode = TRIG_RADAR;
    trig->timer_on = 0;
    trig->start_h = 7;
    trig->start_m = 0;
    trig->end_h = 18;
    trig->end_m = 0;
    fp = fopen(TRIG_FILE, "rb");
    if (fp == NULL) {
        return;
    }
    if (fread(raw, 1, 6, fp) == 6 &&
        (raw[0] == TRIG_RADAR || raw[0] == TRIG_CAMERA) &&
        (raw[1] == 0 || raw[1] == 1) &&
        raw[2] <= 23 && raw[4] <= 23 && raw[3] <= 59 && raw[5] <= 59) {
        trig->mode = raw[0];
        trig->timer_on = raw[1];
        trig->start_h = raw[2];
        trig->start_m = raw[3];
        trig->end_h = raw[4];
        trig->end_m = raw[5];
    }
    fclose(fp);
}

static int trig_save(const struct trig *trig)
{
    FILE *fp;
    uint8_t raw[6];

    raw[0] = (uint8_t)trig->mode;
    raw[1] = (uint8_t)(trig->timer_on ? 1 : 0);
    raw[2] = (uint8_t)trig->start_h;
    raw[3] = (uint8_t)trig->start_m;
    raw[4] = (uint8_t)trig->end_h;
    raw[5] = (uint8_t)trig->end_m;
    fp = fopen(TRIG_FILE, "wb");
    if (fp == NULL) {
        return -1;
    }
    if (fwrite(raw, 1, 6, fp) != 6) {
        fclose(fp);
        return -1;
    }
    return fclose(fp) == 0 ? 0 : -1;
}

void trig_init(struct trig *trig, struct voice *voice, struct led *led,
               const char *dev, unsigned baud)
{
    if (trig == NULL) {
        return;
    }
    memset(trig, 0, sizeof(*trig));
    trig->voice = voice;
    trig->led = led;
    trig->pending = -1;
    trig->queued = -1;
    if (dev != NULL) {
        snprintf(trig->dev, sizeof(trig->dev), "%s", dev);
    }
    trig->baud = baud;
    trig_load(trig);
    current = trig;
}

static int send_bytes(int (*publish)(void *, const uint8_t *, int), void *ctx,
                      const uint8_t *data, int n)
{
    if (n < 0 || publish == NULL || publish(ctx, data, n) != 0) {
        return -1;
    }
    return 0;
}

static int send_ack(int (*publish)(void *, const uint8_t *, int), void *ctx,
                    uint64_t seq, uint8_t origin, uint8_t code)
{
    uint8_t out[16];
    int n;

    n = voice_ack(out, (int)sizeof(out), seq, origin, code);
    return send_bytes(publish, ctx, out, n);
}

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CHDEV_IOC_SET_BAUD _IOW('C', 3, unsigned int)

static int now_allowed(const struct trig *trig)
{
    time_t sec;
    struct tm local;

    if (!trig->timer_on) {
        return 1;
    }
    sec = time(NULL);
    if (localtime_r(&sec, &local) == NULL) {
        return 0;
    }
    return trig_allows(1,
                       trig->start_h * 60 + trig->start_m,
                       trig->end_h * 60 + trig->end_m,
                       local.tm_hour * 60 + local.tm_min);
}

static uint64_t now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

static int event_send(struct trig *trig, int source, uint8_t code,
                      int (*publish)(void *, const uint8_t *, int), void *ctx)
{
    uint8_t out[16];
    uint64_t seq;
    int i;

    seq = now_ms();
    out[0] = TRIG_EVENT;
    for (i = 7; i >= 0; i--) {
        out[1 + (7 - i)] = (uint8_t)((seq >> (i * 8)) & 0xFFu);
    }
    out[9] = (uint8_t)source;
    out[10] = code;
    (void)trig;
    return send_bytes(publish, ctx, out, 11);
}

static void ack_radar(int fd, uint8_t seq)
{
    uint8_t frame[16];
    int n;
    int off;

    if (fd < 0) {
        return;
    }
    n = n32_frame_build(frame, (int)sizeof(frame), N32_CMD_EVENT_ACK, seq, NULL, 0);
    if (n < 0) {
        return;
    }
    off = 0;
    while (off < n) {
        int w = (int)write(fd, frame + off, (size_t)(n - off));

        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return;
        }
        off += w;
    }
}

static int perform(struct trig *trig, int (*pump)(void *, int), void *ctx)
{
    int voice_rc;
    int led_rc;
    int did;
    int failed;

    if (admin_is_busy() || (trig->voice != NULL && trig->voice->busy)) {
        return VOICE_FAIL;
    }
    if (!now_allowed(trig)) {
        log_info("不在允许时段，触发不执行");
        return VOICE_FAIL;
    }
    did = 0;
    failed = 0;
    voice_rc = 1;
    led_rc = 1;
    if (trig->voice != NULL && trig->voice->enabled) {
        voice_rc = voice_play_raw(trig->voice, pump, ctx);
        if (voice_rc != 0) {
            failed = 1;
        }
    }
    if (trig->led != NULL && trig->led->on && trig->led->text_len > 0) {
        led_rc = led_show_raw(trig->led);
        if (led_rc != 0) {
            failed = 1;
        }
    }
    if (voice_rc == 0 || led_rc == 0) {
        did = 1;
    }
    if (!did || failed) {
        return VOICE_FAIL;
    }
    return VOICE_OK;
}

void trig_seen_radar(int fd, uint8_t seq)
{
    if (current == NULL) {
        return;
    }
    ack_radar(fd, seq);
    if (current->mode == TRIG_CAMERA) {
        log_info("当前是摄像机触发，雷达这次忽略");
        return;
    }
    if (current->have_seq && current->last_seq == seq && current->pending < 0) {
        return;
    }
    current->have_seq = 1;
    current->last_seq = seq;
    if (current->acting) {
        current->queued = seq;
        return;
    }
    if (current->pending < 0) {
        current->pending = seq;
    }
}

static void read_port(struct trig *trig)
{
    uint8_t buf[256];
    unsigned baud;
    int fd;
    int used;
    int n;

    if (trig->dev[0] == '\0') {
        return;
    }
    fd = open(trig->dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return;
    }
    baud = trig->baud;
    if (ioctl(fd, CHDEV_IOC_SET_BAUD, &baud) != 0) {
        close(fd);
        return;
    }
    used = 0;
    n = (int)read(fd, buf, sizeof(buf));
    if (n > 0) {
        used = n;
    }
    while (used > 0) {
        struct n32_frame frame;
        int ate;

        n = n32_frame_parse(buf, used, &frame, &ate);
        if (n == 0) {
            break;
        }
        if (ate > 0 && ate <= used) {
            memmove(buf, buf + ate, (size_t)(used - ate));
            used -= ate;
        }
        if (n == 1 && frame.cmd == N32_EVT_RADAR) {
            trig_seen_radar(fd, frame.seq);
        }
    }
    close(fd);
}

void trig_service(struct trig *trig,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    if (trig == NULL) {
        return;
    }
    read_port(trig);
    if (!trig->acting && trig->pending < 0 && trig->queued >= 0) {
        trig->pending = trig->queued;
        trig->queued = -1;
    }
    while (trig->pending >= 0 && !trig->acting) {
        int code;

        trig->acting = 1;
        trig->pending = -1;
        code = perform(trig, pump, ctx);
        trig->acting = 0;
        if (event_send(trig, 1, (uint8_t)code, publish, ctx) != 0) {
            log_error("触发结果没有发出去");
        }
        if (trig->queued >= 0) {
            trig->pending = trig->queued;
            trig->queued = -1;
        }
    }
}

static int run_manual(struct trig *trig, uint64_t seq,
                      int (*publish)(void *, const uint8_t *, int),
                      int (*pump)(void *, int), void *ctx)
{
    int code;

    if ((trig->voice != NULL && trig->voice->busy) || !now_allowed(trig)) {
        if (send_ack(publish, ctx, seq, TRIG_MANUAL, VOICE_FAIL) != 0) {
            return -1;
        }
        return event_send(trig, 2, VOICE_FAIL, publish, ctx);
    }
    if (send_ack(publish, ctx, seq, TRIG_MANUAL, VOICE_STARTED) != 0) {
        return -1;
    }
    trig->acting = 1;
    code = perform(trig, pump, ctx);
    trig->acting = 0;
    if (send_ack(publish, ctx, seq, TRIG_MANUAL, (uint8_t)code) != 0) {
        return -1;
    }
    return event_send(trig, 2, (uint8_t)code, publish, ctx);
}

#else

void trig_seen_radar(int fd, uint8_t seq)
{
    (void)fd;
    (void)seq;
}

void trig_service(struct trig *trig,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    (void)trig;
    (void)publish;
    (void)pump;
    (void)ctx;
}

#endif

int trig_on_packet(struct trig *trig, const uint8_t *buf, int len,
                   int (*publish)(void *ctx, const uint8_t *data, int n),
                   int (*pump)(void *ctx, int idle_ms), void *ctx)
{
#ifdef _WIN32
    (void)trig;
    (void)buf;
    (void)len;
    (void)publish;
    (void)pump;
    (void)ctx;
    return -1;
#else
    struct trig_cmd cmd;
    uint8_t out[16];
    int rc;
    int n;

    if (trig == NULL || publish == NULL) {
        return -1;
    }
    rc = trig_parse(buf, len, &cmd);
    if (rc > 0) {
        return 1;
    }
    if (rc < 0) {
        if (buf != NULL && len >= 9 && buf[0] >= TRIG_MODE && buf[0] <= TRIG_MANUAL) {
            return send_ack(publish, ctx, read_seq(buf), buf[0], VOICE_FAIL);
        }
        return 0;
    }
    if (cmd.cmd == TRIG_QUERY) {
        n = trig_state(out, (int)sizeof(out), cmd.seq, trig);
        return send_bytes(publish, ctx, out, n);
    }
    if (cmd.cmd == TRIG_MANUAL) {
        return run_manual(trig, cmd.seq, publish, pump, ctx);
    }
    if (cmd.cmd == TRIG_MODE) {
        trig->mode = cmd.mode;
    } else {
        trig->timer_on = cmd.timer_on;
        trig->start_h = cmd.start_h;
        trig->start_m = cmd.start_m;
        trig->end_h = cmd.end_h;
        trig->end_m = cmd.end_m;
    }
    if (trig_save(trig) != 0) {
        log_error("触发参数写不进 %s", TRIG_FILE);
        return send_ack(publish, ctx, cmd.seq, cmd.cmd, VOICE_FAIL);
    }
    log_info("触发方式=%d 定时=%d %02d:%02d-%02d:%02d",
             trig->mode, trig->timer_on, trig->start_h, trig->start_m,
             trig->end_h, trig->end_m);
    return send_ack(publish, ctx, cmd.seq, cmd.cmd, VOICE_OK);
#endif
}
