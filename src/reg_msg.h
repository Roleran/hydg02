#ifndef REG_MSG_H
#define REG_MSG_H

#include <stdint.h>

/* MQTT 注册上报 0x81。序列号是 8 字节大端毫秒时间戳。 */
#define REG_CMD 0x81u

/*
 * 成功返回报文长度。身份串为空、超长或缓冲不够返回 -1。
 * 布局：命令 + 序列号 + 长度和内容（IMEI、固件版本、ICCID）。
 */
int reg_build(uint8_t *out, int cap, uint64_t seq_ms,
              const char *imei, const char *firmware, const char *iccid);

/* 注册回复 0x01。code 为 0 时 cid 有效。解析成功返回 0。 */
#define REG_REPLY 0x01u

struct reg_reply {
    uint64_t seq;
    int code;
    char cid[64];
};

int reg_parse(const uint8_t *buf, int len, struct reg_reply *reply);

#endif
