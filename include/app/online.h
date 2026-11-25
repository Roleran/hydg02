/*
 * include/app/online.h - 设备上线模块 (协议 V3.0 第 4 章)
 *
 * 完整流程 (4.4):
 *   HTTPS 获取接入信息 (4.1) → MQTT/TLS 连接 (3.1) → 订阅 auth
 *   → 0x81 注册 → 0x01 回复 → 保存 CID → 订阅 msg/upgrade
 *   → 发布 will 在线 (3.4) → 业务就绪
 *
 * 失败处理 (4.5): 30s 退避, 连续 5 次 60s, 连续 10 次 300s
 */
#ifndef APP_ONLINE_H
#define APP_ONLINE_H

#include <stdint.h>

/* 上线状态 */
enum online_state {
	ONLINE_INIT = 0,
	ONLINE_FETCH_ACCESS,	/* HTTPS 获取接入信息 */
	ONLINE_MQTT_WAIT,	/* 等待 MQTT 连接建立 */
	ONLINE_AUTH_WAIT,	/* 已发 0x81, 等 0x01 */
	ONLINE_ONLINE,		/* 注册成功, 业务就绪 */
	ONLINE_BACKOFF,		/* 退避重试中 */
};

const char *online_state_name(enum online_state s);
enum online_state online_get_state(void);

/* 当前 CID (未注册时空串) */
const char *online_get_cid(void);

/* MQTT 连接句柄 (proto.c 等发上行报文用, 不能传 NULL) */
struct mqtt;
struct mqtt *online_mqtt(void);

/* pump: 由主循环驱动 (处理接入信息获取/注册状态机) */
void online_pump(void);

/* MQTT 收到报文的分发入口 (proto.c 的 mqtt 回调最终进这里) */
void online_on_mqtt_msg(const char *topic, const uint8_t *payload, int len);

#endif /* APP_ONLINE_H */
