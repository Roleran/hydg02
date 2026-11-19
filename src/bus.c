#include "bus.h"

#include "log.h"
#include "voice.h"

#include <stdio.h>
#include <string.h>

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

static void put_u64(uint8_t *out, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        out[7 - i] = (uint8_t)((v >> (i * 8)) & 0xFFu);
    }
}

int bus_parse(const uint8_t *buf, int len, struct bus_cmd *cmd)
{
    int timeout;
    int n;
    int i;

    if (buf == NULL || cmd == NULL || len < 9) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = buf[0];
    cmd->seq = read_seq(buf);
    if (cmd->cmd != BUS_XFER) {
        return 1;
    }
    if (len < 14) {
        return -1;
    }
    cmd->port = buf[9];
    timeout = (buf[10] << 8) | buf[11];
    n = (buf[12] << 8) | buf[13];
    if ((cmd->port != 1 && cmd->port != 2) || timeout < 50 || timeout > 3000 ||
        n <= 0 || n > BUS_DATA || len != 14 + n) {
        return -1;
    }
    cmd->timeout_ms = timeout;
    cmd->data_len = n;
    for (i = 0; i < n; i++) {
        cmd->data[i] = buf[14 + i];
    }
    return 0;
}

int bus_reply(uint8_t *out, int cap, uint64_t seq, uint8_t code,
              const uint8_t *data, int data_len)
{
    int i;

    if (out == NULL || data_len < 0 || data_len > BUS_DATA || cap < 12 + data_len) {
        return -1;
    }
    if (data_len > 0 && data == NULL) {
        return -1;
    }
    out[0] = BUS_REPLY;
    put_u64(out + 1, seq);
    out[9] = code;
    out[10] = (uint8_t)((data_len >> 8) & 0xFF);
    out[11] = (uint8_t)(data_len & 0xFF);
    for (i = 0; i < data_len; i++) {
        out[12 + i] = data[i];
    }
    return 12 + data_len;
}

void bus_init(struct bus *bus, const struct app_config *cfg)
{
    if (bus == NULL) {
        return;
    }
    memset(bus, 0, sizeof(*bus));
    bus->baud = 9600;
    if (cfg == NULL) {
        return;
    }
    snprintf(bus->dev[0], sizeof(bus->dev[0]), "%s", cfg->rs485_dev);
    snprintf(bus->dev[1], sizeof(bus->dev[1]), "%s", cfg->rs485_6_dev);
    if (cfg->rs485_baud > 0) {
        bus->baud = cfg->rs485_baud;
    }
}

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define CHDEV_IOC_SET_BAUD _IOW('C', 3, unsigned int)

static int elapsed_ms(const struct timeval *start)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (int)((now.tv_sec - start->tv_sec) * 1000 +
                 (now.tv_usec - start->tv_usec) / 1000);
}

static int write_all(int fd, const uint8_t *data, int len)
{
    int off;

    off = 0;
    while (off < len) {
        struct pollfd pfd;
        int n;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, 200) <= 0) {
            return -1;
        }
        n = (int)write(fd, data + off, (size_t)(len - off));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        off += n;
    }
    return 0;
}

static void drain(int fd)
{
    uint8_t junk[64];

    while (read(fd, junk, sizeof(junk)) > 0) {
    }
}

/* 发出原始字节，再把随后收到的原始字节收回来。安静 20 毫秒视为一帧结束。 */
static int exchange(int fd, const uint8_t *req, int req_len, uint8_t *resp, int *resp_len,
                    int timeout_ms, int (*pump)(void *, int), void *ctx)
{
    struct timeval start;
    int filled;

    drain(fd);
    if (write_all(fd, req, req_len) != 0) {
        return -1;
    }
    filled = 0;
    gettimeofday(&start, NULL);
    while (elapsed_ms(&start) < timeout_ms && filled < BUS_DATA) {
        struct pollfd pfd;
        int left;
        int slice;
        int n;

        left = timeout_ms - elapsed_ms(&start);
        if (left <= 0) {
            break;
        }
        slice = filled > 0 ? 20 : left;
        if (slice > left) {
            slice = left;
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        n = poll(&pfd, 1, slice);
        if (pump != NULL && pump(ctx, 0) != 0) {
            return -1;
        }
        if (n > 0) {
            n = (int)read(fd, resp + filled, (size_t)(BUS_DATA - filled));
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                return -1;
            }
            filled += n;
            continue;
        }
        if (filled > 0) {
            break;
        }
    }
    *resp_len = filled;
    return 0;
}

static int publish_reply(int (*publish)(void *, const uint8_t *, int), void *ctx,
                         uint64_t seq, uint8_t code, const uint8_t *data, int n)
{
    uint8_t out[12 + BUS_DATA];
    int len;

    len = bus_reply(out, (int)sizeof(out), seq, code, data, n);
    if (len < 0 || publish(ctx, out, len) != 0) {
        return -1;
    }
    return 0;
}

#endif

int bus_on_packet(struct bus *bus, const uint8_t *buf, int len,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  int (*pump)(void *ctx, int idle_ms), void *ctx)
{
#ifdef _WIN32
    (void)bus;
    (void)buf;
    (void)len;
    (void)publish;
    (void)pump;
    (void)ctx;
    return -1;
#else
    struct bus_cmd cmd;
    uint8_t resp[BUS_DATA];
    unsigned baud;
    int resp_len;
    int fd;
    int rc;
    const char *path;

    if (bus == NULL || publish == NULL) {
        return -1;
    }
    rc = bus_parse(buf, len, &cmd);
    if (rc > 0) {
        return 1;
    }
    if (rc < 0) {
        if (buf != NULL && len >= 9 && buf[0] == BUS_XFER) {
            return publish_reply(publish, ctx, read_seq(buf), VOICE_FAIL, NULL, 0);
        }
        return 0;
    }
    if (bus->busy) {
        return publish_reply(publish, ctx, cmd.seq, VOICE_FAIL, NULL, 0);
    }
    path = bus->dev[cmd.port - 1];
    if (path[0] == '\0') {
        return publish_reply(publish, ctx, cmd.seq, VOICE_FAIL, NULL, 0);
    }
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        log_error("打不开 485 口 %s", path);
        return publish_reply(publish, ctx, cmd.seq, VOICE_FAIL, NULL, 0);
    }
    baud = bus->baud;
    if (ioctl(fd, CHDEV_IOC_SET_BAUD, &baud) != 0) {
        log_error("485 波特率设置失败");
        close(fd);
        return publish_reply(publish, ctx, cmd.seq, VOICE_FAIL, NULL, 0);
    }
    bus->busy = 1;
    resp_len = 0;
    rc = exchange(fd, cmd.data, cmd.data_len, resp, &resp_len, cmd.timeout_ms, pump, ctx);
    bus->busy = 0;
    close(fd);
    if (rc != 0) {
        log_error("485 端口 %d 转发失败", cmd.port);
        return publish_reply(publish, ctx, cmd.seq, VOICE_FAIL, NULL, 0);
    }
    log_info("485 端口 %d 发出 %d 字节，收回 %d 字节", cmd.port, cmd.data_len, resp_len);
    return publish_reply(publish, ctx, cmd.seq, VOICE_OK, resp, resp_len);
#endif
}
