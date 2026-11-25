/*
 * app/proto.c - 协议报文分发 (协议 V3.0)
 *
 * 从 online.c 的 MQTT 回调接收下行报文, 按协议 ID 分发。
 * 当前实现: 0x01 注册回复 (提取 CID) + 0x04 下载指令 (转发 OTA)
 *           + 0x02 功能码 0x05 (固件版本查询 / OTA 激活, 6.5)
 * 后续: 0x02 其余功能码 (LED/语音/触发/485 等)
 */
#include <app/proto.h>
#include <app/online.h>
#include <net/mqtt.h>
#include <pal/config.h>
#include <pal/sysio.h>
#include <common/tlv.h>
#include <sal/initcall.h>

#include <stdio.h>
#include <string.h>

/* === Topic 构建 === */

static void topic_resp_msg(char *buf, int cap)
{
	snprintf(buf, cap, "raw/v1/resp/msg/%s", online_get_cid());
}

static void topic_resp_upgrade(char *buf, int cap)
{
	snprintf(buf, cap, "raw/v1/resp/upgrade/%s", online_get_cid());
}

/* === 发送接口 === */

int proto_send_progress(uint64_t seq, uint8_t file_type,
			uint8_t phase, uint8_t progress, uint8_t rc)
{
	uint8_t msg[16];
	char topic[64];
	int off = 0;

	msg[off++] = PROTO_ID_PROGRESS;
	be64_put(msg + off, seq); off += 8;
	msg[off++] = file_type;
	msg[off++] = phase;
	msg[off++] = progress;
	msg[off++] = rc;

	topic_resp_upgrade(topic, sizeof(topic));
	{
		int r = mqtt_publish(online_mqtt(), topic, msg, off, 0, 0);

		/* 实时冲刷: 0x84 常在阻塞操作(下载/校验/安装)期间
		 * 产生, 那时主循环卡住、正常 pump 不跑 — 立即把队
		 * 列写出, 平台才能看到实时进度 */
		mqtt_flush_tx(online_mqtt());
		return r;
	}
}

int proto_send_ack(uint64_t seq, uint8_t func, uint8_t op,
		   uint8_t rc, const uint8_t *tlv, int tlv_len)
{
	uint8_t msg[280];
	char topic[64];
	int off = 0;

	if (tlv_len > 255)
		return -1;

	msg[off++] = PROTO_ID_ACK;
	be64_put(msg + off, seq); off += 8;
	msg[off++] = func;
	msg[off++] = op;
	msg[off++] = rc;
	be16_put(msg + off, tlv_len); off += 2;
	if (tlv_len) {
		memcpy(msg + off, tlv, tlv_len);
		off += tlv_len;
	}

	topic_resp_msg(topic, sizeof(topic));
	return mqtt_publish(online_mqtt(), topic, msg, off, 1, 0);
}

/* === 下行解析 === */

/* 0x01 注册回复 (4.3) */
static void handle_reg_reply(const uint8_t *p, int len)
{
	uint64_t seq;
	uint8_t code;
	char cid[32];
	int cid_len;

	if (len < 11)
		return;

	seq = be64_get(p + 1);
	code = p[9];
	if (code != 0x00) {
		printf("proto: 注册失败 code=%02x\n", code);
		return;
	}

	cid_len = p[10];
	if (cid_len > 30 || 11 + cid_len > len)
		return;
	memcpy(cid, p + 11, cid_len);
	cid[cid_len] = '\0';
	(void)seq;

	online_registered(cid);
}

/* 0x04 文件下载指令 (8.1) */
static void handle_download(const uint8_t *p, int len)
{
	uint64_t seq;
	uint8_t file_type;
	uint16_t param_len;
	const uint8_t *tlv;

	if (len < 12)
		return;

	seq = be64_get(p + 1);
	file_type = p[9];
	param_len = be16_get(p + 10);
	if (12 + param_len > len)
		return;
	tlv = p + 12;

	{
		const uint8_t *url;
		uint16_t url_len;
		const uint8_t *sm3;
		const uint8_t *sig;
		const uint8_t *ver;
		uint16_t ver_len;
		char url_s[256], ver_s[32];

		if (tlv_find(tlv, param_len, 0x10, &url, &url_len) != 0) {
			printf("proto: 0x04 缺 URL\n");
			return;
		}
		if (url_len >= sizeof(url_s))
			return;
		memcpy(url_s, url, url_len);
		url_s[url_len] = '\0';

		sm3 = NULL;
		tlv_find(tlv, param_len, 0x11, &sm3, &url_len);	/* SM3 32B */
		sig = NULL;
		tlv_find(tlv, param_len, 0x24, &sig, &url_len);	/* SM2 64B */
		ver = NULL;
		ver_len = 0;
		if (tlv_find_subtype(tlv, param_len, 0x0F, 0x01,
				     &ver, &ver_len) == 0 && ver_len < 30) {
			memcpy(ver_s, ver, ver_len);
			ver_s[ver_len] = '\0';
		} else {
			ver_s[0] = '\0';
		}

		printf("proto: 0x04 下载 type=%02x ver=%s\n",
		       file_type, ver_s);
		printf("  URL: %s\n", url_s);
		if (sm3)
			printf("  SM3: 有\n");
		if (sig)
			printf("  SM2: 有\n");

		/* 转发到 OTA 模块 (后续接入) */
		extern int ota_start(uint64_t seq, uint8_t file_type,
				     const char *url, const char *ver,
				     const uint8_t *sm3, const uint8_t *sig);
		if (ota_start(seq, file_type, url_s, ver_s, sm3, sig) != 0)
			printf("proto: OTA 拒绝 (忙或参数错误)\n");
	}
}

/* 0x02 通用指令 (2.3) */
static void handle_cmd(const uint8_t *p, int len)
{
	uint64_t seq;
	uint8_t func, op;
	uint16_t param_len;

	if (len < 13)
		return;

	seq = be64_get(p + 1);
	func = p[9];
	op = p[10];
	param_len = be16_get(p + 11);

	(void)seq;
	(void)param_len;

	/* 各功能码的实现在后续版本接入 */
	switch (func) {
	case 0x05:	/* 固件 OTA 升级 (6.5: 查询/激活) */
		if (op == 0x03) {
			/* 激活指令: 参数区 Tag 0x0F 子类型 0x01 = 目标版本号 */
			const uint8_t *tlv = p + 13;
			const uint8_t *ver;
			uint16_t ver_len;
			char ver_s[32];
			int ret;

			if (13 + (int)param_len > len ||
			    tlv_find_subtype(tlv, param_len, 0x0F, 0x01,
					     &ver, &ver_len) != 0 ||
			    ver_len >= sizeof(ver_s)) {
				printf("proto: 激活指令参数错误\n");
				proto_send_ack(seq, 0x05, 0x03,
					       PROTO_RC_PARAM, NULL, 0);
				break;
			}
			memcpy(ver_s, ver, ver_len);
			ver_s[ver_len] = '\0';
			printf("proto: OTA 激活指令 ver=%s\n", ver_s);

			/* 6.5 第 5 步: 校验当前运行版本一致后激活备份槽,
			 * 内部置 slot_x_stable=1 并上报 0x84 成功 */
			extern int ota_activate(const char *version);

			ret = ota_activate(ver_s);
			proto_send_ack(seq, 0x05, 0x03,
				       ret == 0 ? PROTO_RC_OK
						: PROTO_RC_PARAM,
				       NULL, 0);
		} else if (op == 0x01) {
			/* 版本查询 (6.5): 应答 Tag 0x0F 子类型 0x01 + 当前固件版本 */
			const char *fw = conf_get("fw_version", "0.0.0");
			uint8_t tlv_buf[64];
			uint8_t vbuf[40];
			struct tlv_writer w;
			int fw_len = strlen(fw);

			if (fw_len > (int)sizeof(vbuf) - 1)
				fw_len = sizeof(vbuf) - 1;
			vbuf[0] = 0x01;			/* 子类型: 固件版本 */
			memcpy(vbuf + 1, fw, fw_len);
			tlv_writer_init(&w, tlv_buf, sizeof(tlv_buf));
			tlv_add(&w, 0x0F, vbuf, fw_len + 1);
			proto_send_ack(seq, 0x05, 0x01, PROTO_RC_OK,
				       tlv_buf, w.len);
		}
		break;

	case 0x10:	/* 设备状态与维护 */
		if (op == 0x01) {
			printf("proto: 状态查询 (暂未实现)\n");
		}
		break;

	default:
		printf("proto: 功能码 %02x 未实现, 回 0x02\n", func);
		proto_send_ack(seq, func, op, PROTO_RC_UNSUP, NULL, 0);
		break;
	}
}

/* === MQTT 消息入口 === */

void proto_on_mqtt_msg(const char *topic, const uint8_t *payload, int len)
{
	uint8_t id;

	if (len < 1)
		return;

	id = payload[0];

	/* 按方向位判断: 下行 bit7=0 */
	if (id & 0x80) {
		printf("proto: 收到上行方向报文 %02x (异常, 忽略)\n", id);
		return;
	}

	switch (id) {
	case PROTO_ID_REG_REPLY:
		handle_reg_reply(payload, len);
		break;
	case PROTO_ID_CMD:
		handle_cmd(payload, len);
		break;
	case PROTO_ID_DL:
		handle_download(payload, len);
		break;
	case PROTO_ID_MODBUS:
		printf("proto: 0x05 Modbus 透传 (暂未实现)\n");
		break;
	case PROTO_ID_INTERCOM:
		printf("proto: 0x06 对讲控制 (暂未实现)\n");
		break;
	default:
		printf("proto: 未知下行 %02x\n", id);
		break;
	}
}

/* === initcall === */

static int proto_sysinit(void)
{
	/* 注册到 online 的 MQTT 回调链 (后续: online 调 proto_on_mqtt_msg) */
	return 0;
}
initcall_app(proto_sysinit);
