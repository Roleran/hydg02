#ifndef LED_H
#define LED_H

#include "config.h"

#include <stdint.h>

/* 平台下发。屏上的点阵由 N32 完成，这里只保存参数并转发。 */
#define LED_SWITCH 0x40u
#define LED_TEXT   0x41u
#define LED_SPEED  0x42u
#define LED_ONCE   0x43u
#define LED_QUERY  0x44u

/* 查询结果。走向不在报文里，固定由右往左。 */
#define LED_STATE  0xA2u

#define LED_TEXT_MAX 120

struct led_cmd {
    uint8_t cmd;
    uint64_t seq;
    int on;
    int speed;
    uint8_t text[LED_TEXT_MAX];
    int text_len;
};

struct led {
    int on;
    int speed;
    uint8_t text[LED_TEXT_MAX];
    int text_len;
    char dev[64];
    unsigned baud;
};

/* 是本命令返回 0，别的命令返回 1，字段非法返回 -1。 */
int led_parse(const uint8_t *buf, int len, struct led_cmd *cmd);

int led_state(uint8_t *out, int cap, uint64_t seq, int on, int speed,
              const uint8_t *text, int text_len);

void led_init(struct led *led, const struct app_config *cfg);

/* 注册成功后把已保存的开关和文字写到屏上。失败只记日志。 */
void led_sync(struct led *led);

/* 滚一遍当前文字。0 已开始，1 开关关或没有文字，-1 屏没接受。不发平台回执。 */
int led_show_raw(struct led *led);

/*
 * 不是 LED 命令返回 1。处理完返回 0。回执发不出去返回 -1。
 */
int led_on_packet(struct led *led, const uint8_t *buf, int len,
                  int (*publish)(void *ctx, const uint8_t *data, int n),
                  void *ctx);

#endif
