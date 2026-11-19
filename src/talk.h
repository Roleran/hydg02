#ifndef TALK_H
#define TALK_H

#include "voice.h"

#include <stdint.h>

/* 平台下发。参考协议要求走 ZLMediaKit，字节以本仓库第 3 章为准。 */
#define TALK_START 0x30u
#define TALK_STOP  0x31u

/* 挂在 0xA0 的结果码上。0x00、0x01、0x02 与语音播报相同。 */
#define TALK_TIMEOUT 0x04u
#define TALK_BROKEN  0x05u

#define TALK_HOST_MAX 63

struct talk_cmd {
    uint8_t cmd;
    uint64_t seq;
    int port;
    char host[TALK_HOST_MAX + 1];
};

struct talk {
    struct voice *voice;
    int active;
    int stop_req;
    uint64_t start_seq;
};

/* 是对讲命令返回 0，别的命令返回 1，字段非法返回 -1。序号仍会填上。 */
int talk_parse(const uint8_t *buf, int len, struct talk_cmd *cmd);

void talk_init(struct talk *talk, struct voice *voice);

/*
 * 不是对讲命令返回 1。处理完返回 0。MQTT 断了返回 -1。
 * pump 的 idle_ms 小于 0 时只发心跳。
 */
int talk_on_packet(struct talk *talk, const uint8_t *buf, int len,
                   int (*publish)(void *ctx, const uint8_t *data, int n),
                   int (*pump)(void *ctx, int idle_ms), void *ctx);

#endif
