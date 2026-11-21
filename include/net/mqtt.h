/*
 * include/net/mqtt.h - MQTT 3.1.1 客户端 (协议 V3.0 3.1)
 *
 * TLS 8883 + 证书链校验 + username/password + CleanSession=0
 * + 遗嘱(raw/v1/will/{IMEI}, QoS1, retained) + QoS0/1 收发 + pump 驱动。
 *
 * 单实例设计 (设备只有一个 MQTT 连接), 非阻塞,
 * 由主循环 mqtt_pump() 驱动 (连接/收发/PINGREQ/重连退避)。
 */
#ifndef NET_MQTT_H
#define NET_MQTT_H

#include <stdint.h>
#include <stddef.h>

/* 连接状态 */
enum mqtt_state {
	MQTT_INIT = 0,
	MQTT_TLS_CONNECTING,
	MQTT_CONNECTING,	/* 已发 CONNECT, 等 CONNACK */
	MQTT_CONNECTED,
	MQTT_RECONNECT_WAIT,	/* 退避中 */
};

/* 收到一条 PUBLISH */
typedef void (*mqtt_msg_fn)(void *ctx, const char *topic,
			    const uint8_t *payload, int len);

/* 连接建立 / 断开 (CONNACK 成功 / 异常断线) */
typedef void (*mqtt_conn_fn)(void *ctx);

struct mqtt {
	/* 配置 (init 前填好) */
	char host[64];
	int port;
	char client_id[32];
	char username[48];
	char password[64];
	char ca_cert_path[96];		/* 平台根证书 PEM */
	char will_topic[64];		/* raw/v1/will/{IMEI} */
	uint8_t will_payload[16];	/* 0x83 遗嘱报文 */
	int will_len;

	/* 回调 */
	mqtt_msg_fn on_msg;
	mqtt_conn_fn on_connected;
	mqtt_conn_fn on_disconnected;
	void *cb_ctx;

	/* 运行状态 (内部) */
	enum mqtt_state state;
	uint64_t last_rx_ms;		/* 最近收包时间 */
	uint64_t last_ping_ms;
	uint64_t reconnect_at_ms;
	int fail_count;			/* 退避计数: 30s/60s/300s (4.5) */
	uint16_t next_pkt_id;
	uint16_t pend_acks[8];		/* QoS1 在途 PUBACK 跟踪 */
	int npend;
	uint8_t rxbuf[4096];
	int rxlen;
	uint8_t txbuf[2048];
	int txlen;
	void *ssl;			/* mbedtls 上下文 (mqtt_cli 内部) */
	int connected_once;
};

void mqtt_init(struct mqtt *m);
/* 设置回调 */
void mqtt_set_handler(struct mqtt *m, mqtt_msg_fn fn, void *ctx);

/* 主循环驱动: 处理 TLS 握手/收包/发送/PINGREQ/重连退避。
 * 每次调用开销极小 (非阻塞)。 */
void mqtt_pump(struct mqtt *m);

/* 发布。QoS 0 或 1。返回 0=已受理(入发送队列), -1=忙/未连接/缓冲满。 */
int mqtt_publish(struct mqtt *m, const char *topic,
		 const uint8_t *payload, int len, int qos, int retain);

/* 订阅 (QoS1)。返回 0=已发订阅, -1=失败。 */
int mqtt_subscribe(struct mqtt *m, const char *topic);

int mqtt_is_connected(struct mqtt *m);

/* 主动断开 (发 DISCONNECT, 不触发遗嘱) */
void mqtt_disconnect(struct mqtt *m);

#endif /* NET_MQTT_H */
