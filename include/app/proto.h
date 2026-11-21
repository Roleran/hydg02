/*
 * include/app/proto.h - 协议报文分发 (协议 V3.0 2.x)
 *
 * MQTT 报文解析/组装/分发。下行: 0x01/0x02/0x04/0x05/0x06
 * 上行: 0x81/0x82/0x83/0x84/0x86/0x87
 */
#ifndef APP_PROTO_H
#define APP_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* 0x84 文件类型 */
#define PROTO_FILE_FW		0x01
#define PROTO_FILE_ALGO_HUMAN	0x02
#define PROTO_FILE_ALGO_FIRE	0x03
#define PROTO_FILE_MP3		0x04

/* 0x84 阶段状态 */
#define PROTO_PHASE_RECVED	0x01
#define PROTO_PHASE_DOWNLOAD	0x02
#define PROTO_PHASE_VERIFY	0x03
#define PROTO_PHASE_INSTALL	0x04
#define PROTO_PHASE_REBOOT	0x05
#define PROTO_PHASE_SUCCESS	0x06
#define PROTO_PHASE_FAIL	0x07

/* 2.6 结果码 */
#define PROTO_RC_OK		0x00
#define PROTO_RC_PARAM		0x01
#define PROTO_RC_UNSUP		0x02
#define PROTO_RC_BUSY		0x03
#define PROTO_RC_CHECKSUM	0x04
#define PROTO_RC_DOWNLOAD	0x05
#define PROTO_RC_TIMEOUT	0x06
#define PROTO_RC_AUTH		0x07
#define PROTO_RC_OTHER		0x7F

/* 协议 ID */
#define PROTO_ID_REG_REPLY	0x01	/* 下行: 注册回复 */
#define PROTO_ID_CMD		0x02	/* 下行: 通用指令 */
#define PROTO_ID_DL		0x04	/* 下行: 文件下载 */
#define PROTO_ID_MODBUS		0x05	/* 下行: Modbus 透传 */
#define PROTO_ID_INTERCOM	0x06	/* 下行: 对讲控制 */
#define PROTO_ID_REG		0x81	/* 上行: 注册 */
#define PROTO_ID_ACK		0x82	/* 上行: 通用应答 */
#define PROTO_ID_STATUS		0x83	/* 上行: 状态上报 */
#define PROTO_ID_PROGRESS	0x84	/* 上行: 下载/升级进度 */
#define PROTO_ID_EVENT		0x85	/* 上行: 事件 */
#define PROTO_ID_MODBUS_RESP	0x86	/* 上行: Modbus 响应 */
#define PROTO_ID_INTERCOM_STAT	0x87	/* 上行: 对讲状态 */

/* MQTT 消息入口 (online.c 的 on_msg 最终到这里) */
void proto_on_mqtt_msg(const char *topic, const uint8_t *payload, int len);

/* 发 0x84 进度上报 (OTA 模块调用) */
int proto_send_progress(uint64_t seq, uint8_t file_type,
			uint8_t phase, uint8_t progress, uint8_t rc);

/* 发 0x82 通用应答 */
int proto_send_ack(uint64_t seq, uint8_t func, uint8_t op,
		   uint8_t rc, const uint8_t *tlv, int tlv_len);

/* 注册成功 (proto 解析 0x01 后通知 online) */
void online_registered(const char *cid);

#endif /* APP_PROTO_H */
