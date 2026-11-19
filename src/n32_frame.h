#ifndef N32_FRAME_H
#define N32_FRAME_H

#include <stdint.h>

/* 与《N32与MT串口协议》0.1 一致。DATA 最长 128，文字最长 120。 */
#define N32_VER          0x01u
#define N32_DATA_MAX     128
#define N32_TEXT_MAX     120
#define N32_FRAME_MAX    (9 + N32_DATA_MAX)

#define N32_CMD_PING     0x01u
#define N32_CMD_VERSION  0x02u
#define N32_CMD_SWITCH   0x10u
#define N32_CMD_TEXT     0x11u
#define N32_CMD_ONCE     0x12u
#define N32_CMD_QUERY    0x13u
#define N32_CMD_ACK      0x80u
#define N32_CMD_EVENT_ACK 0x8Fu
#define N32_EVT_RADAR    0x90u
#define N32_EVT_DONE     0x91u

/*
 * 整机只有由右往左。发给 N32 的走向字节固定用 0x01。
 * 屏程序按由右往左处理，不再使用原稿里的上下方向，也不新增 0x03。
 */
#define N32_DIR_RTL      0x01u

struct n32_frame {
    uint8_t cmd;
    uint8_t seq;
    uint8_t data[N32_DATA_MAX];
    int len;
};

/* CRC-16/MODBUS，覆盖从 VER 到 DATA。 */
uint16_t n32_crc16(const uint8_t *buf, int len);

/* 成功返回帧长，缓冲不够或长度非法返回 -1。 */
int n32_frame_build(uint8_t *out, int cap, uint8_t cmd, uint8_t seq,
                    const uint8_t *data, int len);

/*
 * 从流里取一帧。
 * 返回 1：*used 是吃掉的字节数，*frame 有效。
 * 返回 0：字节还不够。
 * 返回 -1：丢掉了无效字节，*used 是丢掉的数量，调用方继续喂剩下的。
 */
int n32_frame_parse(const uint8_t *buf, int len, struct n32_frame *frame, int *used);

#endif
