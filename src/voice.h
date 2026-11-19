#ifndef VOICE_H
#define VOICE_H

#include "config.h"

#include <stdint.h>

/* 平台下发，Topic raw/v1/cmd/msg/{CID}。参考协议没有这些命令号。 */
#define VOICE_SET    0x20u
#define VOICE_QUERY  0x21u
#define VOICE_FETCH  0x22u
#define VOICE_CLEAR  0x23u
#define VOICE_PLAY   0x24u

/* 设备回执。0xA0 是执行结果，0xA1 是查询结果。 */
#define VOICE_RESULT 0xA0u
#define VOICE_STATE  0xA1u

#define VOICE_OK      0x00u
#define VOICE_STARTED 0x01u
#define VOICE_FAIL    0x02u
#define VOICE_BAD_MD5 0x03u

#define VOICE_URL_MAX 480
#define VOICE_SLOTS   3

struct voice_cmd {
    uint8_t cmd;
    uint64_t seq;
    int on;
    int slot;
    uint8_t md5[16];
    char url[VOICE_URL_MAX + 1];
};

struct voice {
    char dir[192];
    int amp_gpio;
    int enabled;
    int busy;
};

/* 已知命令返回 0，命令号不认识返回 1，截断或字段非法返回 -1。 */
int voice_parse(const uint8_t *buf, int len, struct voice_cmd *cmd);

/* 后缀 wav/mp3/pcm/opus 返回 1，否则返回 0。 */
int voice_known_suffix(const char *url);

int voice_ack(uint8_t *out, int cap, uint64_t seq, uint8_t origin, uint8_t code);
int voice_state(uint8_t *out, int cap, uint64_t seq, int on, int count);

void voice_init(struct voice *voice, const struct app_config *cfg);

/*
 * 处理一条业务指令。publish 把回执发出去。
 * pump 在下载和播报期间让出时间，返回非 0 表示链路已断。
 * 链路断开返回 -1，指令已处理返回 0。
 */
/* 拉高或拉低功放。失败返回 -1。 */
int voice_amp(struct voice *voice, int on);

/* 按槽位播一遍。0 播完，1 开关关或没有文件，-1 失败。不发平台回执。 */
int voice_play_raw(struct voice *voice, int (*pump)(void *ctx, int idle_ms), void *ctx);

int voice_on_packet(struct voice *voice, const uint8_t *buf, int len,
                    int (*publish)(void *ctx, const uint8_t *data, int n),
                    int (*pump)(void *ctx, int idle_ms), void *ctx);

#endif
