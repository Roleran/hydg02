#ifndef ADMIN_H
#define ADMIN_H

#include "config.h"

#include <stdint.h>

struct voice;
struct bus;

#define ADMIN_UPGRADE 0x70u
#define ADMIN_REBOOT  0x71u
#define ADMIN_RESET   0x72u
#define ADMIN_VERSION 0x73u
#define ADMIN_VER_REPLY 0xA6u
#define ADMIN_URL_MAX 480

struct admin_cmd {
    uint8_t cmd;
    uint64_t seq;
    uint8_t md5[16];
    char url[ADMIN_URL_MAX + 1];
};

struct admin {
    struct voice *voice;
    struct bus *bus;
    char firmware[64];
    char voice_dir[192];
    int busy;
};

int admin_parse(const uint8_t *buf, int len, struct admin_cmd *cmd);
int admin_version(uint8_t *out, int cap, uint64_t seq, const char *firmware);
int admin_is_busy(void);

void admin_init(struct admin *admin, const struct app_config *cfg,
                struct voice *voice, struct bus *bus);

/* 业务主题上的重启、恢复出厂和版本查询。不是这些命令返回 1。 */
int admin_on_packet(struct admin *admin, const uint8_t *buf, int len,
                    int (*publish)(void *ctx, const uint8_t *data, int n),
                    void *ctx);

/* 升级主题上的 0x70。回执由调用方发到 upgrade 应答主题。 */
int admin_on_upgrade(struct admin *admin, const uint8_t *buf, int len,
                     int (*publish)(void *ctx, const uint8_t *data, int n),
                     int (*pump)(void *ctx, int idle_ms), void *ctx);

#endif
