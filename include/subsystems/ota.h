/*
 * include/subsystems/ota.h - OTA 升级子系统 (协议 V3.0 6.5/8.x)
 *
 * 固件下载(SM3+SM2 校验) -> AB 双槽安装 -> 切槽重启 -> 激活/回退
 */
#ifndef SUBSYS_OTA_H
#define SUBSYS_OTA_H

#include <stdint.h>

/* 启动 OTA 下载 (proto.c 0x04 转发)。
 * 返回 0=已受理, -1=忙或参数错误。 */
int ota_start(uint64_t seq, uint8_t file_type,
	      const char *url, const char *ver,
	      const uint8_t *sm3, const uint8_t *sm2_sig);

/* OTA 是否进行中 (其他子系统查询做互锁) */
int ota_busy(void);

/* pump: 主循环驱动 */
void ota_pump(void);

/* 激活指令处理 (proto.c 0x02/0x05/0x03 转发)。
 * version: 目标版本号字符串。返回 0=激活成功, -1=版本不匹配。 */
int ota_activate(const char *version);

#endif /* SUBSYS_OTA_H */
