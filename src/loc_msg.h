#ifndef LOC_MSG_H
#define LOC_MSG_H

#include "gps_fix.h"

#include <stdint.h>

/* 注册成功后的定位上报。参考协议没有这一帧，字节以本仓库说明为准。 */
#define LOC_CMD 0x82u

/*
 * 固定 22 字节：命令、8 字节大端毫秒时间戳、有效标志、
 * 纬度 int32、经度 int32（1e-7 度，北/东为正）、UTC 时分秒 uint32。
 * 没有定位或度分格式无法换算时，有效标志为 0，后三个数都是 0。
 * 缓冲不够返回 -1。
 */
int loc_build(uint8_t *out, int cap, uint64_t seq_ms, const struct gps_fix *fix);

#endif
