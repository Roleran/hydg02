#include "mqtt_cli.h"

#ifndef _WIN32

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct mqtt {
    int fd;
    unsigned char rx[4096];
    int rx_len;
    unsigned short packet_id;
};

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int tcp_connect(const char *host, int port)
{
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *it;
    int fd;
    int rc;

    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, port_text, &hints, &res);
    if (rc != 0) {
        return -1;
    }
    fd = -1;
    for (it = res; it != NULL; it = it->ai_next) {
        struct pollfd pfd;
        int err;
        socklen_t err_len;

        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (set_nonblock(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc == 0) {
            break;
        }
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, 10000) <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        err = 0;
        err_len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int write_all(int fd, const unsigned char *buf, int len)
{
    int off;

    off = 0;
    while (off < len) {
        struct pollfd pfd;
        int n;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, 5000) <= 0) {
            return -1;
        }
        n = (int)write(fd, buf + off, (size_t)(len - off));
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

static int put_rl(unsigned char *out, int n)
{
    int i;

    i = 0;
    do {
        unsigned char b = (unsigned char)(n % 128);
        n /= 128;
        if (n > 0) {
            b = (unsigned char)(b | 0x80u);
        }
        out[i++] = b;
    } while (n > 0 && i < 4);
    return i;
}

static int put_str(unsigned char *out, const char *text)
{
    int n;

    n = (int)strlen(text);
    out[0] = (unsigned char)((n >> 8) & 0xFF);
    out[1] = (unsigned char)(n & 0xFF);
    memcpy(out + 2, text, (size_t)n);
    return 2 + n;
}

static int read_more(struct mqtt *mq, int timeout_ms)
{
    struct pollfd pfd;
    unsigned char tmp[512];
    int n;

    if (mq->rx_len >= (int)sizeof(mq->rx)) {
        return -1;
    }
    pfd.fd = mq->fd;
    pfd.events = POLLIN;
    n = poll(&pfd, 1, timeout_ms);
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    n = (int)read(mq->fd, tmp, sizeof(tmp));
    if (n == 0) {
        return -1;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (mq->rx_len + n > (int)sizeof(mq->rx)) {
        return -1;
    }
    memcpy(mq->rx + mq->rx_len, tmp, (size_t)n);
    mq->rx_len += n;
    return 1;
}

/* 从缓冲里取出一帧。返回 1 成功，0 还不够，-1 坏包。 */
static int take_packet(struct mqtt *mq, unsigned char *type, unsigned char *body,
                       int *body_len, int body_cap)
{
    int rl;
    int mul;
    int i;
    int hdr;
    int total;

    if (mq->rx_len < 2) {
        return 0;
    }
    rl = 0;
    mul = 1;
    i = 1;
    for (;;) {
        unsigned char b;

        if (i >= mq->rx_len) {
            return 0;
        }
        b = mq->rx[i++];
        rl += (b & 0x7Fu) * mul;
        if ((b & 0x80u) == 0) {
            break;
        }
        mul *= 128;
        if (mul > 128 * 128 * 128) {
            return -1;
        }
    }
    hdr = i;
    total = hdr + rl;
    if (rl < 0 || rl > body_cap || total > (int)sizeof(mq->rx)) {
        return -1;
    }
    if (mq->rx_len < total) {
        return 0;
    }
    *type = (unsigned char)(mq->rx[0] >> 4);
    memcpy(body, mq->rx + hdr, (size_t)rl);
    *body_len = rl;
    memmove(mq->rx, mq->rx + total, (size_t)(mq->rx_len - total));
    mq->rx_len -= total;
    return 1;
}

struct mqtt *mqtt_connect(const char *host, int port, const char *client_id)
{
    struct mqtt *mq;
    unsigned char pkt[256];
    int pos;
    int rl_n;
    unsigned char rl[4];
    unsigned char type;
    unsigned char body[16];
    int body_len;
    int left;

    if (host == NULL || host[0] == '\0' || client_id == NULL || client_id[0] == '\0') {
        return NULL;
    }
    mq = calloc(1, sizeof(*mq));
    if (mq == NULL) {
        return NULL;
    }
    mq->fd = tcp_connect(host, port);
    if (mq->fd < 0) {
        free(mq);
        return NULL;
    }
    mq->packet_id = 1;

    pkt[0] = 0x10;
    pos = 5;
    pos += put_str(pkt + pos, "MQTT");
    pkt[pos++] = 4;
    pkt[pos++] = 0x02;
    pkt[pos++] = 0;
    pkt[pos++] = 60;
    pos += put_str(pkt + pos, client_id);
    rl_n = put_rl(rl, pos - 5);
    memmove(pkt + 1 + rl_n, pkt + 5, (size_t)(pos - 5));
    memcpy(pkt + 1, rl, (size_t)rl_n);
    if (write_all(mq->fd, pkt, 1 + rl_n + (pos - 5)) != 0) {
        mqtt_close(mq);
        return NULL;
    }

    left = 10000;
    while (left > 0) {
        int step = left > 500 ? 500 : left;
        int got = read_more(mq, step);
        int taken;

        left -= step;
        if (got < 0) {
            mqtt_close(mq);
            return NULL;
        }
        taken = take_packet(mq, &type, body, &body_len, (int)sizeof(body));
        if (taken == 0) {
            continue;
        }
        if (taken < 0 || type != 2 || body_len < 2 || body[1] != 0) {
            mqtt_close(mq);
            return NULL;
        }
        return mq;
    }
    mqtt_close(mq);
    return NULL;
}

int mqtt_subscribe(struct mqtt *mq, const char *topic)
{
    unsigned char pkt[256];
    int pos;
    unsigned char rl[4];
    int rl_n;
    unsigned char type;
    unsigned char body[8];
    int body_len;
    int left;

    if (mq == NULL || topic == NULL) {
        return -1;
    }
    pkt[0] = 0x82;
    pos = 5;
    mq->packet_id++;
    pkt[pos++] = (unsigned char)(mq->packet_id >> 8);
    pkt[pos++] = (unsigned char)(mq->packet_id & 0xFF);
    pos += put_str(pkt + pos, topic);
    pkt[pos++] = 0;
    rl_n = put_rl(rl, pos - 5);
    memmove(pkt + 1 + rl_n, pkt + 5, (size_t)(pos - 5));
    memcpy(pkt + 1, rl, (size_t)rl_n);
    if (write_all(mq->fd, pkt, 1 + rl_n + (pos - 5)) != 0) {
        return -1;
    }

    left = 10000;
    while (left > 0) {
        int step = left > 500 ? 500 : left;
        int got = read_more(mq, step);
        int taken;

        left -= step;
        if (got < 0) {
            return -1;
        }
        taken = take_packet(mq, &type, body, &body_len, (int)sizeof(body));
        if (taken == 0) {
            continue;
        }
        if (taken < 0 || type != 9) {
            return -1;
        }
        return 0;
    }
    return -1;
}

int mqtt_publish(struct mqtt *mq, const char *topic,
                 const unsigned char *payload, int len)
{
    unsigned char pkt[1024];
    int pos;
    unsigned char rl[4];
    int rl_n;

    if (mq == NULL || topic == NULL || len < 0 || 5 + 2 + (int)strlen(topic) + len > (int)sizeof(pkt)) {
        return -1;
    }
    pkt[0] = 0x30;
    pos = 5;
    pos += put_str(pkt + pos, topic);
    if (len > 0) {
        memcpy(pkt + pos, payload, (size_t)len);
        pos += len;
    }
    rl_n = put_rl(rl, pos - 5);
    memmove(pkt + 1 + rl_n, pkt + 5, (size_t)(pos - 5));
    memcpy(pkt + 1, rl, (size_t)rl_n);
    return write_all(mq->fd, pkt, 1 + rl_n + (pos - 5));
}

static int decode_publish(const unsigned char *body, int body_len,
                          char *topic, int topic_cap,
                          unsigned char *payload, int *payload_len, int payload_cap)
{
    int tlen;
    int i;

    if (body_len < 2) {
        return -1;
    }
    tlen = (body[0] << 8) | body[1];
    if (tlen < 0 || 2 + tlen > body_len || tlen >= topic_cap) {
        return -1;
    }
    for (i = 0; i < tlen; i++) {
        topic[i] = (char)body[2 + i];
    }
    topic[tlen] = '\0';
    *payload_len = body_len - 2 - tlen;
    if (*payload_len > payload_cap) {
        return -1;
    }
    if (*payload_len > 0) {
        memcpy(payload, body + 2 + tlen, (size_t)*payload_len);
    }
    return 0;
}

int mqtt_wait_publish(struct mqtt *mq, char *topic, int topic_cap,
                      unsigned char *payload, int *payload_len, int payload_cap,
                      int timeout_ms)
{
    unsigned char type;
    unsigned char body[1024];
    int body_len;
    int left;

    if (mq == NULL) {
        return -1;
    }
    /* 超时为 0 时只取缓冲里已经到的报文，不发心跳、也不再读套接字。 */
    if (timeout_ms <= 0) {
        for (;;) {
            int taken = take_packet(mq, &type, body, &body_len, (int)sizeof(body));

            if (taken <= 0) {
                return taken;
            }
            if (type == 3) {
                if (decode_publish(body, body_len, topic, topic_cap,
                                   payload, payload_len, payload_cap) != 0) {
                    return -1;
                }
                return 1;
            }
        }
    }
    left = timeout_ms;
    while (left > 0) {
        int step = left > 500 ? 500 : left;
        int got;
        int taken;

        taken = take_packet(mq, &type, body, &body_len, (int)sizeof(body));
        if (taken < 0) {
            return -1;
        }
        if (taken == 1) {
            if (type == 3) {
                if (decode_publish(body, body_len, topic, topic_cap,
                                   payload, payload_len, payload_cap) != 0) {
                    return -1;
                }
                return 1;
            }
            if (type == 13 || type == 14) {
                continue;
            }
            continue;
        }
        got = read_more(mq, step);
        left -= step;
        if (got < 0) {
            return -1;
        }
    }
    return 0;
}

int mqtt_ping(struct mqtt *mq)
{
    unsigned char pkt[2];

    pkt[0] = 0xC0;
    pkt[1] = 0x00;
    if (mq == NULL) {
        return -1;
    }
    return write_all(mq->fd, pkt, 2);
}

void mqtt_close(struct mqtt *mq)
{
    if (mq == NULL) {
        return;
    }
    if (mq->fd >= 0) {
        close(mq->fd);
    }
    free(mq);
}

#else

struct mqtt {
    int unused;
};

struct mqtt *mqtt_connect(const char *host, int port, const char *client_id)
{
    (void)host;
    (void)port;
    (void)client_id;
    return NULL;
}

int mqtt_subscribe(struct mqtt *mq, const char *topic)
{
    (void)mq;
    (void)topic;
    return -1;
}

int mqtt_publish(struct mqtt *mq, const char *topic,
                 const unsigned char *payload, int len)
{
    (void)mq;
    (void)topic;
    (void)payload;
    (void)len;
    return -1;
}

int mqtt_wait_publish(struct mqtt *mq, char *topic, int topic_cap,
                      unsigned char *payload, int *payload_len, int payload_cap,
                      int timeout_ms)
{
    (void)mq;
    (void)topic;
    (void)topic_cap;
    (void)payload;
    (void)payload_len;
    (void)payload_cap;
    (void)timeout_ms;
    return -1;
}

int mqtt_ping(struct mqtt *mq)
{
    (void)mq;
    return -1;
}

void mqtt_close(struct mqtt *mq)
{
    (void)mq;
}

#endif
