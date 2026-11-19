#ifndef BUS_H
#define BUS_H

#include "config.h"

#include <stdint.h>

/* 两路 RS485 原样转发。不解析 Modbus。 */
#define BUS_XFER   0x60u
#define BUS_REPLY  0xA5u
#define BUS_DATA   256

struct bus_cmd {
    uint8_t cmd;
    uint64_t seq;
    int port;
    int timeout_ms;
    uint8_t data[BUS_DATA];
    int data_len;
};

struct bus {
    char dev[2][64];
    unsigned baud;
    int busy;
};

/* 是本命令返回 0，别的命令返回 1，字段非法返回 -1。 */
int bus_parse(const uint8_t *buf, int len, struct bus_cmd *cmd);

/* 应答：序号 + 结果码 + 长度 + 原始字节。没有现场应答时长度为 0。 */
int bus_reply(uint8_t *out, int cap, uint64_t seq, uint8_t code,
              const uint8_t *data, int data_len);

void bus_init(struct bus *bus, const struct app_config *cfg);

int bus_on_packet(struct bus *bus, const uint8_t *buf, int len,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  int (*pump)(void *ctx, int idle_ms), void *ctx);

#endif
