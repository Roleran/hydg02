/*
 * app/online.c - 设备上线模块 (协议 V3.0 第 4 章)
 *
 * 状态机: FETCH_ACCESS -> MQTT_WAIT -> AUTH_WAIT -> ONLINE
 * 任何失败 -> BACKOFF (30s/60s/300s 递增) -> FETCH_ACCESS 重来
 *
 * 接入信息 (4.1): HTTPS GET {base_url}/api/v1/mqtt/access?imei={IMEI}
 * 返回 JSON: hostPrimary/hostBackup/domain/port/clientId/username/password
 *
 * 注册 (4.2): 0x81 -> raw/v1/resp/auth/{IMEI}
 * 回复 (4.3): 0x01 <- raw/v1/cmd/auth/{IMEI}, code=0x00 携带 CID
 *
 * will (3.4): 遗嘱 = 0x83/type 0x09 (CONNECT 时设置)
 * 在线消息: 0x83/type 0x00 -> raw/v1/will/{IMEI} (retained)
 */
#include <app/online.h>
#include <app/proto.h>
#include <net/mqtt.h>
#include <net/https.h>
#include <pal/config.h>
#include <pal/sysio.h>
#include <common/tlv.h>
#include <sal/initcall.h>
#include <sal/evbus.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* === 状态 === */

static enum online_state st = ONLINE_INIT;
static char g_cid[32];			/* 平台分配的业务标识 */
static uint64_t backoff_until_ms;
static int fail_count;
static uint64_t last_progress_ms;	/* 状态打印节流 */

static struct mqtt M;			/* 单实例 MQTT 连接 */

/* === 协议报文构造 === */

/* 0x81 注册上报 (4.2) */
static int build_reg_msg(uint8_t *buf, int cap)
{
	const char *imei = conf_get("imei", "000000000000001");
	const char *fw = conf_get("fw_version", "10.01.01");
	const char *iccid = conf_get("iccid", "");
	uint64_t seq = sys_seq();
	int off = 0, len;

	buf[off++] = 0x81;
	be64_put(buf + off, seq); off += 8;

	len = strlen(imei);
	buf[off++] = (uint8_t)len;
	memcpy(buf + off, imei, len); off += len;

	len = strlen(fw);
	buf[off++] = (uint8_t)len;
	memcpy(buf + off, fw, len); off += len;

	len = strlen(iccid);
	buf[off++] = (uint8_t)len;
	memcpy(buf + off, iccid, len); off += len;

	(void)cap;
	return off;
}

/* 0x83 状态上报 (5.2): type + 参数区(TLV) */
static int build_status_msg(uint8_t *buf, int cap, uint8_t type,
			    const uint8_t *tlv, int tlv_len)
{
	uint64_t seq = sys_seq();
	int off = 0;

	buf[off++] = 0x83;
	be64_put(buf + off, seq); off += 8;
	buf[off++] = type;
	be16_put(buf + off, tlv_len); off += 2;
	if (tlv_len) {
		memcpy(buf + off, tlv, tlv_len);
		off += tlv_len;
	}
	(void)cap;
	return off;
}

/* === 4.1 接入信息解析 (极简 JSON 键提取) === */

static void json_get_str(const char *json, const char *key,
			 char *out, int cap)
{
	char pattern[64];
	const char *p;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p) {
		out[0] = '\0';
		return;
	}
	p = strchr(p + strlen(pattern), ':');
	if (!p) {
		out[0] = '\0';
		return;
	}
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '"') {
		p++;
		{
			int n = 0;

			while (p[n] && p[n] != '"' && n < cap - 1)
				n++;
			memcpy(out, p, n);
			out[n] = '\0';
		}
	} else {
		out[0] = '\0';
	}
}

static int json_get_int(const char *json, const char *key, int def)
{
	char s[16];

	json_get_str(json, key, s, sizeof(s));
	return s[0] ? atoi(s) : def;
}

/* === MQTT 回调 === */

static void on_mqtt_connected(void *ctx)
{
	(void)ctx;
	printf("online: MQTT 已连接, 开始注册\n");
	st = ONLINE_AUTH_WAIT;

	/* 4.4: 先订阅 auth 再发 0x81 */
	{
		char topic[64];
		const char *imei = conf_get("imei", "000000000000001");

		snprintf(topic, sizeof(topic), "raw/v1/cmd/auth/%s", imei);
		mqtt_subscribe(&M, topic);
	}

	/* 发 0x81 */
	{
		char topic[64];
		uint8_t msg[128];
		int len;
		const char *imei = conf_get("imei", "000000000000001");

		len = build_reg_msg(msg, sizeof(msg));
		snprintf(topic, sizeof(topic), "raw/v1/resp/auth/%s", imei);
		mqtt_publish(&M, topic, msg, len, 1, 0);
		printf("online: 0x81 已发 (IMEI=%s)\n", imei);
	}
}

static void on_mqtt_disconnected(void *ctx)
{
	(void)ctx;
	if (st == ONLINE_ONLINE) {
		printf("online: 断线, 重新上线\n");
		st = ONLINE_MQTT_WAIT;
		fail_count = 0;
	}
}

static void on_mqtt_msg(void *ctx, const char *topic,
			const uint8_t *payload, int len)
{
	(void)ctx;
	/* 协议解析转发到 proto.c */
	proto_on_mqtt_msg(topic, payload, len);
}

/* proto.c 解析到 0x01 成功时调用 */
void online_registered(const char *cid)
{
	strncpy(g_cid, cid, sizeof(g_cid) - 1);
	g_cid[sizeof(g_cid) - 1] = '\0';
	st = ONLINE_ONLINE;
	fail_count = 0;

	printf("online: 注册成功 CID=%s\n", g_cid);

	/* 4.4: 订阅业务 Topic */
	{
		char topic[64];

		snprintf(topic, sizeof(topic), "raw/v1/cmd/msg/%s", g_cid);
		mqtt_subscribe(&M, topic);
		snprintf(topic, sizeof(topic), "raw/v1/cmd/upgrade/%s", g_cid);
		mqtt_subscribe(&M, topic);
	}

	/* 3.4: 发布在线消息 (retained) */
	{
		char topic[64];
		uint8_t msg[16];
		int len;
		const char *imei = conf_get("imei", "000000000000001");

		len = build_status_msg(msg, sizeof(msg), 0x00, NULL, 0);
		snprintf(topic, sizeof(topic), "raw/v1/will/%s", imei);
		mqtt_publish(&M, topic, msg, len, 1, 1);
		printf("online: will 在线消息已发\n");
	}
}

/* === 状态机 pump === */

void online_pump(void)
{
	uint64_t now = sys_ms();

	switch (st) {
	case ONLINE_INIT:
		st = ONLINE_FETCH_ACCESS;
		break;

	case ONLINE_FETCH_ACCESS: {
		/* 4.1: HTTPS 获取接入信息 */
		const char *base_url = conf_get("ota_server", "");
		char url[192], json[1024];
		char host[48], user[32], pass[32];
		int port, body_len = 0, ret;
		const char *imei = conf_get("imei", "000000000000001");
		const char *ca = conf_get("ca_cert", "");

		if (!base_url[0]) {
			printf("online: ota_server 未配置, 无法上线\n");
			st = ONLINE_BACKOFF;
			backoff_until_ms = now + 30000;
			return;
		}

		snprintf(url, sizeof(url), "%s/api/v1/mqtt/access?imei=%s",
			 base_url, imei);
		ret = https_get_mem(url, ca, json, sizeof(json), &body_len);
		if (ret != 200) {
			printf("online: 接入信息获取失败 (%d)\n", ret);
			goto retry;
		}
		json[body_len] = '\0';

		json_get_str(json, "hostPrimary", host, sizeof(host));
		port = json_get_int(json, "port", 8883);
		json_get_str(json, "username", user, sizeof(user));
		json_get_str(json, "password", pass, sizeof(pass));

		if (!host[0]) {
			printf("online: 接入信息缺 hostPrimary\n");
			goto retry;
		}

		/* 配置 MQTT 连接 */
		mqtt_init(&M);
		strncpy(M.host, host, sizeof(M.host) - 1);
		M.port = port;
		strncpy(M.client_id, imei, sizeof(M.client_id) - 1);
		strncpy(M.username, user, sizeof(M.username) - 1);
		strncpy(M.password, pass, sizeof(M.password) - 1);
		strncpy(M.ca_cert_path, ca, sizeof(M.ca_cert_path) - 1);
		snprintf(M.will_topic, sizeof(M.will_topic),
			 "raw/v1/will/%s", imei);
		{
			int wl = build_status_msg(M.will_payload,
						  sizeof(M.will_payload),
						  0x09, NULL, 0);

			M.will_len = wl;
		}
		M.on_msg = on_mqtt_msg;
		M.on_connected = on_mqtt_connected;
		M.on_disconnected = on_mqtt_disconnected;

		printf("online: 接入 %s:%d user=%s\n", host, port, user);
		st = ONLINE_MQTT_WAIT;
		break;
	}

	case ONLINE_MQTT_WAIT:
		/* mqtt_pump 驱动连接, 连上后回调进入 AUTH_WAIT */
		if (now - last_progress_ms > 10000) {
			last_progress_ms = now;
			printf("online: 等待 MQTT 连接...\n");
		}
		break;

	case ONLINE_AUTH_WAIT:
		/* 等待 0x01 (proto.c 解析后调 online_registered) */
		if (now - last_progress_ms > 10000) {
			last_progress_ms = now;
			printf("online: 等待注册回复 0x01...\n");
		}
		break;

	case ONLINE_ONLINE:
		/* 业务就绪, proto.c 处理报文 */
		break;

	case ONLINE_BACKOFF:
		if (now >= backoff_until_ms) {
			printf("online: 重试上线 (第 %d 次失败后)\n",
			       fail_count);
			st = ONLINE_FETCH_ACCESS;
		}
		break;
	}

	/* MQTT 驱动 (FETCH_ACCESS 之后的阶段) */
	if (st >= ONLINE_MQTT_WAIT)
		mqtt_pump(&M);

	return;

retry:
	fail_count++;
	{
		int delay = (fail_count >= 10) ? 300000 :
			    (fail_count >= 5) ? 60000 : 30000;

		st = ONLINE_BACKOFF;
		backoff_until_ms = now + delay;
		printf("online: %ds 后重试\n", delay / 1000);
	}
}

/* === 查询接口 === */

const char *online_get_cid(void)
{
	return g_cid;
}

/* MQTT 连接句柄: proto.c 等模块发上行报文用
 * (proto 的 0x82/0x84 由此取实例, 不能传 NULL) */
struct mqtt *online_mqtt(void)
{
	return &M;
}

enum online_state online_get_state(void)
{
	return st;
}

const char *online_state_name(enum online_state s)
{
	switch (s) {
	case ONLINE_INIT:		return "INIT";
	case ONLINE_FETCH_ACCESS:	return "FETCH_ACCESS";
	case ONLINE_MQTT_WAIT:		return "MQTT_WAIT";
	case ONLINE_AUTH_WAIT:		return "AUTH_WAIT";
	case ONLINE_ONLINE:		return "ONLINE";
	case ONLINE_BACKOFF:		return "BACKOFF";
	}
	return "?";
}

/* === initcall + pumpcall 注册 === */

static int online_sysinit(void)
{
	/* 仅注册, 不主动启动 (pump 驱动状态机) */
	return 0;
}
initcall_app(online_sysinit);

static void online_syspump(void)
{
	online_pump();
}
pumpcall(online_syspump);
