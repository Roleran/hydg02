#include "led.h"

#include "log.h"
#include "n32_frame.h"
#include "trig.h"
#include "voice.h"

#include <stdio.h>
#include <string.h>

#define LED_FILE "/etc/hydg02/led.bin"
#define LED_SPEED_DEFAULT 80

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

int led_parse(const uint8_t *buf, int len, struct led_cmd *cmd)
{
    int n;
    int i;

    if (buf == NULL || cmd == NULL || len < 9) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = buf[0];
    cmd->seq = read_seq(buf);
    if (cmd->cmd != LED_SWITCH && cmd->cmd != LED_TEXT && cmd->cmd != LED_SPEED &&
        cmd->cmd != LED_ONCE && cmd->cmd != LED_QUERY) {
        return 1;
    }
    if (cmd->cmd == LED_ONCE || cmd->cmd == LED_QUERY) {
        return len == 9 ? 0 : -1;
    }
    if (len < 10) {
        return -1;
    }
    if (cmd->cmd == LED_SWITCH) {
        if (len != 10 || (buf[9] != 0 && buf[9] != 1)) {
            return -1;
        }
        cmd->on = buf[9];
        return 0;
    }
    if (cmd->cmd == LED_SPEED) {
        if (len != 10 || buf[9] < 20 || buf[9] > 200) {
            return -1;
        }
        cmd->speed = buf[9];
        return 0;
    }
    n = buf[9];
    if (n > LED_TEXT_MAX || len != 10 + n) {
        return -1;
    }
    cmd->text_len = n;
    for (i = 0; i < n; i++) {
        cmd->text[i] = buf[10 + i];
    }
    return 0;
}

int led_state(uint8_t *out, int cap, uint64_t seq, int on, int speed,
              const uint8_t *text, int text_len)
{
    int i;

    if (out == NULL || text_len < 0 || text_len > LED_TEXT_MAX || cap < 12 + text_len) {
        return -1;
    }
    if (text_len > 0 && text == NULL) {
        return -1;
    }
    out[0] = LED_STATE;
    for (i = 7; i >= 0; i--) {
        out[1 + (7 - i)] = (uint8_t)((seq >> (i * 8)) & 0xFFu);
    }
    out[9] = (uint8_t)(on ? 1 : 0);
    out[10] = (uint8_t)speed;
    out[11] = (uint8_t)text_len;
    for (i = 0; i < text_len; i++) {
        out[12 + i] = text[i];
    }
    return 12 + text_len;
}

static void led_clear(struct led *led)
{
    memset(led, 0, sizeof(*led));
    led->speed = LED_SPEED_DEFAULT;
}

static void led_load(struct led *led)
{
    FILE *fp;
    uint8_t raw[3 + LED_TEXT_MAX];
    int n;

    led->on = 0;
    led->speed = LED_SPEED_DEFAULT;
    led->text_len = 0;
    fp = fopen(LED_FILE, "rb");
    if (fp == NULL) {
        return;
    }
    n = (int)fread(raw, 1, sizeof(raw), fp);
    fclose(fp);
    if (n < 3 || (raw[0] != 0 && raw[0] != 1) || raw[1] < 20 || raw[1] > 200) {
        return;
    }
    if (raw[2] > LED_TEXT_MAX || n != 3 + raw[2]) {
        return;
    }
    led->on = raw[0];
    led->speed = raw[1];
    led->text_len = raw[2];
    memcpy(led->text, raw + 3, (size_t)led->text_len);
}

static int led_save(const struct led *led)
{
    FILE *fp;
    uint8_t raw[3 + LED_TEXT_MAX];

    raw[0] = (uint8_t)(led->on ? 1 : 0);
    raw[1] = (uint8_t)led->speed;
    raw[2] = (uint8_t)led->text_len;
    memcpy(raw + 3, led->text, (size_t)led->text_len);
    fp = fopen(LED_FILE, "wb");
    if (fp == NULL) {
        return -1;
    }
    if (fwrite(raw, 1, (size_t)(3 + led->text_len), fp) != (size_t)(3 + led->text_len)) {
        fclose(fp);
        return -1;
    }
    return fclose(fp) == 0 ? 0 : -1;
}

void led_init(struct led *led, const struct app_config *cfg)
{
    if (led == NULL) {
        return;
    }
    led_clear(led);
    if (cfg != NULL) {
        snprintf(led->dev, sizeof(led->dev), "%s", cfg->n32_dev);
        led->baud = cfg->n32_baud;
    }
    led_load(led);
}

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHDEV_IOC_SET_BAUD _IOW('C', 3, unsigned int)

static uint8_t next_seq(void)
{
    static uint8_t seq = 1;
    uint8_t now;

    now = seq;
    seq = (uint8_t)(seq == 255 ? 1 : seq + 1);
    return now;
}

static int send_ack(int (*publish)(void *, const uint8_t *, int), void *ctx,
                    uint64_t seq, uint8_t origin, uint8_t code)
{
    uint8_t out[16];
    int n;

    n = voice_ack(out, (int)sizeof(out), seq, origin, code);
    if (n < 0 || publish(ctx, out, n) != 0) {
        return -1;
    }
    return 0;
}

/* 等 N32 的 0x80。返回 0 时 *result 是结果码。雷达事件先记下，留给第 5 章。 */
static int wait_ack(int fd, uint8_t seq, uint8_t origin, int *result)
{
    uint8_t buf[256];
    int used;
    int left;

    used = 0;
    left = 300;
    while (left > 0) {
        struct pollfd pfd;
        struct n32_frame frame;
        int ate;
        int n;
        int step;

        step = left > 50 ? 50 : left;
        pfd.fd = fd;
        pfd.events = POLLIN;
        n = poll(&pfd, 1, step);
        left -= step;
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            continue;
        }
        if (used >= (int)sizeof(buf)) {
            used = 0;
        }
        n = (int)read(fd, buf + used, (size_t)(sizeof(buf) - (size_t)used));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        used += n;
        while (used > 0) {
            n = n32_frame_parse(buf, used, &frame, &ate);
            if (n == 0) {
                break;
            }
            if (ate > 0 && ate <= used) {
                memmove(buf, buf + ate, (size_t)(used - ate));
                used -= ate;
            }
            if (n != 1) {
                continue;
            }
            if (frame.cmd == N32_EVT_RADAR) {
                trig_seen_radar(fd, frame.seq);
                continue;
            }
            if (frame.cmd == N32_CMD_ACK && frame.seq == seq && frame.len >= 2 &&
                frame.data[0] == origin) {
                *result = frame.data[1];
                return 0;
            }
        }
    }
    return -1;
}

static int n32_transact(struct led *led, uint8_t cmd, const uint8_t *data, int len,
                        int *result)
{
    uint8_t frame[N32_FRAME_MAX];
    uint8_t seq;
    unsigned baud;
    int fd;
    int n;
    int attempt;

    if (led == NULL || led->dev[0] == '\0') {
        return -1;
    }
    seq = next_seq();
    n = n32_frame_build(frame, (int)sizeof(frame), cmd, seq, data, len);
    if (n < 0) {
        return -1;
    }
    fd = open(led->dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        log_error("打不开屏口 %s", led->dev);
        return -1;
    }
    baud = led->baud;
    if (ioctl(fd, CHDEV_IOC_SET_BAUD, &baud) != 0) {
        log_error("屏口波特率设置失败");
        close(fd);
        return -1;
    }
    for (attempt = 0; attempt < 3; attempt++) {
        int off;

        off = 0;
        while (off < n) {
            struct pollfd pfd;
            int w;

            pfd.fd = fd;
            pfd.events = POLLOUT;
            if (poll(&pfd, 1, 100) <= 0) {
                close(fd);
                return -1;
            }
            w = (int)write(fd, frame + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                close(fd);
                return -1;
            }
            off += w;
        }
        if (wait_ack(fd, seq, cmd, result) == 0) {
            close(fd);
            return 0;
        }
    }
    close(fd);
    log_error("屏没有回复命令 %02X", cmd);
    return -1;
}

static int push_text(struct led *led)
{
    uint8_t data[2 + LED_TEXT_MAX];
    int result;

    data[0] = N32_DIR_RTL;
    data[1] = (uint8_t)led->speed;
    memcpy(data + 2, led->text, (size_t)led->text_len);
    if (n32_transact(led, N32_CMD_TEXT, data, 2 + led->text_len, &result) != 0) {
        return -1;
    }
    return result == 0 ? 0 : -1;
}

int led_show_raw(struct led *led)
{
    int result;

    if (led == NULL) {
        return -1;
    }
    if (!led->on || led->text_len == 0) {
        return 1;
    }
    if (n32_transact(led, N32_CMD_ONCE, NULL, 0, &result) != 0 || result != 0) {
        return -1;
    }
    log_info("屏开始滚动");
    return 0;
}

static int push_switch(struct led *led)
{
    uint8_t data[1];
    int result;

    data[0] = (uint8_t)(led->on ? 1 : 0);
    if (n32_transact(led, N32_CMD_SWITCH, data, 1, &result) != 0) {
        return -1;
    }
    return result == 0 ? 0 : -1;
}

void led_sync(struct led *led)
{
    if (led == NULL) {
        return;
    }
    if (push_text(led) != 0 || push_switch(led) != 0) {
        log_error("启动时同步屏幕失败，平台再下发一次即可");
    }
}

int led_on_packet(struct led *led, const uint8_t *buf, int len,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  void *ctx)
{
    struct led_cmd cmd;
    struct led saved;
    uint8_t out[16 + LED_TEXT_MAX];
    int result;
    int rc;
    int n;

    if (led == NULL || publish == NULL) {
        return -1;
    }
    rc = led_parse(buf, len, &cmd);
    if (rc > 0) {
        return 1;
    }
    if (rc < 0) {
        if (buf != NULL && len >= 9 && buf[0] >= LED_SWITCH && buf[0] <= LED_QUERY) {
            return send_ack(publish, ctx, read_seq(buf), buf[0], VOICE_FAIL);
        }
        return 0;
    }
    if (cmd.cmd == LED_QUERY) {
        n = led_state(out, (int)sizeof(out), cmd.seq, led->on, led->speed,
                      led->text, led->text_len);
        if (n < 0 || publish(ctx, out, n) != 0) {
            return -1;
        }
        return 0;
    }
    saved = *led;
    if (cmd.cmd == LED_SWITCH) {
        led->on = cmd.on;
        if (push_switch(led) != 0) {
            *led = saved;
            return send_ack(publish, ctx, cmd.seq, LED_SWITCH, VOICE_FAIL);
        }
    } else if (cmd.cmd == LED_TEXT) {
        memcpy(led->text, cmd.text, (size_t)cmd.text_len);
        led->text_len = cmd.text_len;
        if (push_text(led) != 0) {
            *led = saved;
            return send_ack(publish, ctx, cmd.seq, LED_TEXT, VOICE_FAIL);
        }
    } else if (cmd.cmd == LED_SPEED) {
        led->speed = cmd.speed;
        if (push_text(led) != 0) {
            *led = saved;
            return send_ack(publish, ctx, cmd.seq, LED_SPEED, VOICE_FAIL);
        }
    } else {
        result = led_show_raw(led);
        return send_ack(publish, ctx, cmd.seq, LED_ONCE,
                        result == 0 ? VOICE_OK : VOICE_FAIL);
    }
    if (led_save(led) != 0) {
        *led = saved;
        push_text(led);
        push_switch(led);
        log_error("屏幕参数写不进 %s", LED_FILE);
        return send_ack(publish, ctx, cmd.seq, cmd.cmd, VOICE_FAIL);
    }
    log_info("屏幕参数已更新，开关=%d 速度=%d 字数=%d", led->on, led->speed, led->text_len);
    return send_ack(publish, ctx, cmd.seq, cmd.cmd, VOICE_OK);
}

#else

void led_sync(struct led *led)
{
    (void)led;
}

int led_on_packet(struct led *led, const uint8_t *buf, int len,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  void *ctx)
{
    (void)led;
    (void)buf;
    (void)len;
    (void)publish;
    (void)ctx;
    return -1;
}

#endif
