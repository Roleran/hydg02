/*
 * net/mqtt_cli.c - MQTT 3.1.1 客户端 over TLS (协议 V3.0 3.1)
 *
 * 传输: TLS 8883 + 平台根证书校验 (mbedtls 2.16, 静态链接)
 * 会话: CleanSession=0, KeepAlive=60s, username/password 认证,
 *       遗嘱 raw/v1/will/{IMEI} (0x83/0x09, QoS1, retained)
 * QoS:  收发均支持 0/1 (PUBACK 双向)
 * 驱动: 非阻塞 pump, 断线按 4.5 退避 (30s -> 60s -> 300s)
 *
 * TLS 握手由 pump 逐步推进; TCP 连接本身为一次性阻塞调用
 * (接入信息给 IP, 无 DNS; 局域网/4G 建连耗时可接受)。
 */
#include <net/mqtt.h>
#include <pal/sysio.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>


/* === mbedtls 上下文 (单实例) === */

struct mqtt_ssl_ctx {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_entropy_context entropy;
	int inited;
};

static struct mqtt_ssl_ctx SS;

static void mqtt_tls_cleanup(struct mqtt *m)
{
	if (!SS.inited)
		return;
	mbedtls_ssl_close_notify(&SS.ssl);
	mbedtls_net_free(&SS.net);
	mbedtls_ssl_free(&SS.ssl);
	mbedtls_ssl_config_free(&SS.conf);
	mbedtls_x509_crt_free(&SS.ca);
	mbedtls_ctr_drbg_free(&SS.ctr_drbg);
	mbedtls_entropy_free(&SS.entropy);
	memset(&SS, 0, sizeof(SS));
	m->ssl = NULL;
}

/* 启动连接: TCP(阻塞) + 初始化 TLS 上下文。
 * 返回 0=进入握手阶段, -1=失败(进入退避)。 */
static int mqtt_tls_connect(struct mqtt *m)
{
	const char *pers = "hydg02-mqtt";
	char portstr[8];

	mqtt_tls_cleanup(m);

	mbedtls_net_init(&SS.net);
	mbedtls_ssl_init(&SS.ssl);
	mbedtls_ssl_config_init(&SS.conf);
	mbedtls_x509_crt_init(&SS.ca);
	mbedtls_ctr_drbg_init(&SS.ctr_drbg);
	mbedtls_entropy_init(&SS.entropy);
	SS.inited = 1;

	snprintf(portstr, sizeof(portstr), "%d", m->port ? m->port : 8883);

	if (mbedtls_ctr_drbg_seed(&SS.ctr_drbg, mbedtls_entropy_func,
				  &SS.entropy,
				  (const unsigned char *)pers, strlen(pers)) != 0)
		goto fail;

	if (m->ca_cert_path[0] &&
	    mbedtls_x509_crt_parse_file(&SS.ca, m->ca_cert_path) != 0) {
		printf("mqtt: CA 证书解析失败: %s\n", m->ca_cert_path);
		goto fail;
	}

	if (mbedtls_ssl_config_defaults(&SS.conf,
					MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		goto fail;

	/* 协议 3.1: 校验证书链, 禁止不校验 */
	mbedtls_ssl_conf_authmode(&SS.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&SS.conf, &SS.ca, NULL);
	mbedtls_ssl_conf_rng(&SS.conf, mbedtls_ctr_drbg_random, &SS.ctr_drbg);

	if (mbedtls_ssl_setup(&SS.ssl, &SS.conf) != 0)
		goto fail;

	/* SNI + hostname 校验 (IP 接入时 mbedtls 也校验 CN/SAN) */
	mbedtls_ssl_set_hostname(&SS.ssl, m->host);

	if (mbedtls_net_connect(&SS.net, m->host, portstr,
				MBEDTLS_NET_PROTO_TCP) != 0) {
		printf("mqtt: TCP 连接失败 %s:%s\n", m->host, portstr);
		goto fail;
	}

	/* 阻塞式 BIO (握手/写已被验证可靠); 读循环用 poll 守卫
	 * 实现有限等待 — 不用 recv_timeout BIO (其超时语义在
	 * 流式 TLS 上有状态隐患, 实测导致收不到后续数据) */
	mbedtls_ssl_set_bio(&SS.ssl, &SS.net,
			    mbedtls_net_send, mbedtls_net_recv, NULL);

	m->ssl = &SS;
	return 0;

fail:
	mqtt_tls_cleanup(m);
	return -1;
}

/* === MQTT 报文编码 === */

/* 剩余长度 varint 编码, 返回写入字节数 */
static int enc_remlen(uint8_t *p, int len)
{
	int n = 0;
	uint8_t byte;

	do {
		byte = len % 128;
		len /= 128;
		if (len > 0)
			byte |= 0x80;
		p[n++] = byte;
	} while (len > 0);
	return n;
}

static int enc_str(uint8_t *p, const char *s)
{
	int len = strlen(s);

	p[0] = (uint8_t)(len >> 8);
	p[1] = (uint8_t)len;
	memcpy(p + 2, s, len);
	return 2 + len;
}

/* 发送 MQTT CONNECT (CleanSession=0, KeepAlive=60, 遗嘱+认证) */
static int mqtt_send_connect(struct mqtt *m)
{
	uint8_t vh[10];
	uint8_t payload[600];
	int plen = 0, remlen, rln;
	uint8_t hdr[5];

	/* 变长头: 协议名/级别/标志/KeepAlive */
	vh[0] = 0x00; vh[1] = 0x04;
	memcpy(vh + 2, "MQTT", 4);
	vh[6] = 0x04;				/* level 4 (3.1.1) */
	vh[7] = 0xC6;				/* user+pass+will QoS1+will retain+will */
	/* bit1 clean session = 0 */
	vh[8] = 0; vh[9] = 60;			/* KeepAlive */

	plen += enc_str(payload + plen, m->client_id);
	plen += enc_str(payload + plen, m->will_topic);
	payload[plen++] = (uint8_t)(m->will_len >> 8);
	payload[plen++] = (uint8_t)m->will_len;
	memcpy(payload + plen, m->will_payload, m->will_len);
	plen += m->will_len;
	plen += enc_str(payload + plen, m->username);
	plen += enc_str(payload + plen, m->password);

	remlen = sizeof(vh) + plen;
	hdr[0] = 0x10;				/* CONNECT */
	rln = 1 + enc_remlen(hdr + 1, remlen);

	/* 组包: hdr + vh + payload */
	{
		uint8_t pkt[640];
		int off = 0;

		memcpy(pkt, hdr, rln); off += rln;
		memcpy(pkt + off, vh, sizeof(vh)); off += sizeof(vh);
		memcpy(pkt + off, payload, plen); off += plen;

		{
			int ret = mbedtls_ssl_write(&SS.ssl, pkt, off);

			if (ret != off) {
				printf("mqtt: CONNECT 发送失败 (%d)\n", ret);
				return -1;
			}
		}
	}

	printf("mqtt: CONNECT 已发 (clientId=%s will=%s)\n",
	       m->client_id, m->will_topic);
	return 0;
}

static uint16_t next_pkt_id(struct mqtt *m)
{
	m->next_pkt_id++;
	if (m->next_pkt_id == 0)
		m->next_pkt_id = 1;
	return m->next_pkt_id;
}

/* 轻量冲刷: 只把待发队列写出, 不读不推进状态机。
 * 供阻塞操作期间(下载/校验/安装)实时发出 0x84 进度 —
 * 此时主循环卡在阻塞调用里, 正常 pump 不会运行。 */
void mqtt_flush_tx(struct mqtt *m)
{
	int ret;

	if (m->state != MQTT_CONNECTED || m->txlen <= 0)
		return;

	ret = mbedtls_ssl_write(&SS.ssl, m->txbuf, m->txlen);
	if (ret > 0) {
		memmove(m->txbuf, m->txbuf + ret, m->txlen - ret);
		m->txlen -= ret;
	}
	/* WANT_WRITE/错误: 留给 pump 下一轮重试 */
}

/* === 报文发送接口 === */

int mqtt_publish(struct mqtt *m, const char *topic,
		 const uint8_t *payload, int len, int qos, int retain)
{
	uint8_t pkt[1200];
	int off = 0, tlen = strlen(topic);
	uint8_t type;

	if (m->state != MQTT_CONNECTED)
		return -1;
	if (len > 900 || tlen > 120)
		return -1;

	type = 0x30 | (qos ? 0x02 : 0) | (retain ? 0x01 : 0);

	/* 固定头 (先留 5 字节编码空间, 前移) */
	off = 5;
	off += enc_str(pkt + off, topic);
	if (qos) {
		uint16_t id = next_pkt_id(m);

		pkt[off++] = (uint8_t)(id >> 8);
		pkt[off++] = (uint8_t)id;
		/* 在途跟踪 */
		if (m->npend < 8)
			m->pend_acks[m->npend++] = id;
	}
	if (len) {
		memcpy(pkt + off, payload, len);
		off += len;
	}

	/* 回填剩余长度 */
	{
		int remlen = off - 5;
		uint8_t rl[4];
		int rln = enc_remlen(rl, remlen);
		int hdr = 5 - 1 - rln;		/* type + varint 起点 */

		pkt[hdr] = type;
		memcpy(pkt + hdr + 1, rl, rln);
		memmove(pkt, pkt + hdr, off - hdr);
		off -= hdr;
	}

	/* 入发送队列 (追加到未发完数据之后) */
	if (m->txlen + off > (int)sizeof(m->txbuf))
		return -1;
	memcpy(m->txbuf + m->txlen, pkt, off);
	m->txlen += off;

	/* 立即尝试冲刷 */
	{
		int wrote = mbedtls_ssl_write(&SS.ssl, m->txbuf, m->txlen);

		if (wrote > 0) {
			memmove(m->txbuf, m->txbuf + wrote, m->txlen - wrote);
			m->txlen -= wrote;
		}
	}
	return 0;
}

int mqtt_subscribe(struct mqtt *m, const char *topic)
{
	uint8_t pkt[160];
	uint16_t id = next_pkt_id(m);
	int off, tlen = strlen(topic);

	if (m->state != MQTT_CONNECTED || tlen > 120)
		return -1;

	pkt[0] = 0x82;				/* SUBSCRIBE */
	pkt[1] = 2 + 2 + tlen + 1;		/* 剩余长度 (<128) */
	pkt[2] = (uint8_t)(id >> 8);
	pkt[3] = (uint8_t)id;
	off = enc_str(pkt + 4, topic) + 4;
	pkt[off++] = 1;				/* QoS 1 */

	{
		int ret = mbedtls_ssl_write(&SS.ssl, pkt, off);

		if (ret != off)
			return -1;
	}
	return 0;
}

void mqtt_disconnect(struct mqtt *m)
{
	uint8_t pkt[2] = { 0xE0, 0x00 };

	if (m->state == MQTT_CONNECTED)
		mbedtls_ssl_write(&SS.ssl, pkt, 2);
	mqtt_tls_cleanup(m);
	m->state = MQTT_INIT;
}

int mqtt_is_connected(struct mqtt *m)
{
	return m->state == MQTT_CONNECTED;
}

/* === 收包解析 === */

static void handle_publish(struct mqtt *m, uint8_t flags,
			   const uint8_t *body, int remlen)
{
	int tlen = (body[0] << 8) | body[1];
	int qos = (flags >> 1) & 3;
	const char *topic;
	const uint8_t *payload;
	int plen;

	if (2 + tlen > remlen)
		return;
	topic = (const char *)body + 2;
	payload = body + 2 + tlen;
	plen = remlen - 2 - tlen;

	if (qos == 1) {
		uint16_t pid;
		uint8_t ack[4];

		if (plen < 2)
			return;
		pid = (payload[0] << 8) | payload[1];
		payload += 2;
		plen -= 2;

		ack[0] = 0x40; ack[1] = 0x02;
		ack[2] = (uint8_t)(pid >> 8);
		ack[3] = (uint8_t)pid;
		mbedtls_ssl_write(&SS.ssl, ack, 4);
	}

	if (m->on_msg) {
		/* topic 临时截断 (回调内不需 NUL: 复制一份) */
		char tbuf[128];

		if (tlen < (int)sizeof(tbuf)) {
			memcpy(tbuf, topic, tlen);
			tbuf[tlen] = '\0';
			m->on_msg(m->cb_ctx, tbuf, payload, plen);
		}
	}
}

static void handle_puback(struct mqtt *m, const uint8_t *body, int remlen)
{
	uint16_t pid;
	int i;

	if (remlen < 2)
		return;
	pid = (body[0] << 8) | body[1];
	for (i = 0; i < m->npend; i++) {
		if (m->pend_acks[i] == pid) {
			m->pend_acks[i] = m->pend_acks[--m->npend];
			break;
		}
	}
}

/* 处理 rxbuf 中的完整报文, 返回消耗的字节数 */
static int mqtt_parse(struct mqtt *m)
{
	uint8_t *b = m->rxbuf;
	int have = m->rxlen;
	int pos = 0;

	while (have - pos >= 2) {
		uint8_t type = b[pos] >> 4;
		uint8_t flags = b[pos] & 0x0F;
		int remlen = 0, mult = 1, vlen = 0;
		const uint8_t *body;

		/* 剩余长度 varint */
		do {
			if (vlen > 3 || have - pos < 2 + vlen + 1)
				return pos;	/* 不完整 */
			remlen += (b[pos + 1 + vlen] & 0x7F) * mult;
			mult *= 128;
			vlen++;
		} while (b[pos + vlen] & 0x80);

		if (have - pos < 1 + vlen + remlen)
			return pos;		/* 不完整 */

		body = b + pos + 1 + vlen;

		switch (type) {
		case 2:				/* CONNACK */
			if (remlen >= 2 && body[1] == 0) {
				m->state = MQTT_CONNECTED;
				m->last_rx_ms = sys_ms();
				m->last_ping_ms = m->last_rx_ms;
				m->fail_count = 0;
				printf("mqtt: CONNACK 成功\n");
				if (m->on_connected)
					m->on_connected(m->cb_ctx);
			} else {
				printf("mqtt: CONNACK 拒绝 rc=%d\n",
				       remlen >= 2 ? body[1] : -1);
				m->state = MQTT_RECONNECT_WAIT;
				m->reconnect_at_ms = sys_ms() + 30000;
				mqtt_tls_cleanup(m);
				return have;
			}
			break;

		case 3:				/* PUBLISH */
			handle_publish(m, flags, body, remlen);
			break;

		case 4:				/* PUBACK */
			handle_puback(m, body, remlen);
			break;

		case 9:				/* SUBACK */
			break;

		case 13:			/* PINGRESP */
			break;

		default:
			break;
		}

		pos += 1 + vlen + remlen;
	}
	return pos;
}

/* === 主循环驱动 === */

void mqtt_pump(struct mqtt *m)
{
	uint64_t now = sys_ms();
	int ssl_err = 0;	/* 断连原因: mbedtls 错误码 (0=超时判定) */

	switch (m->state) {
	case MQTT_INIT:
	case MQTT_RECONNECT_WAIT:
		if (now >= m->reconnect_at_ms && m->host[0]) {
			if (mqtt_tls_connect(m) == 0) {
				m->state = MQTT_TLS_CONNECTING;
			} else {
				/* 4.5 退避: 30s, 连续 5 次 60s, 连续 10 次 300s */
				int delay;

				m->fail_count++;
				if (m->fail_count >= 10)
					delay = 300000;
				else if (m->fail_count >= 5)
					delay = 60000;
				else
					delay = 30000;
				m->reconnect_at_ms = now + delay;
				printf("mqtt: 连接失败, %ds 后重试 (第 %d 次)\n",
				       delay / 1000, m->fail_count);
			}
		}
		break;

	case MQTT_TLS_CONNECTING: {
		int ret = mbedtls_ssl_handshake(&SS.ssl);

		if (ret == 0) {
			printf("mqtt: TLS 握手完成\n");
			if (mqtt_send_connect(m) == 0)
				m->state = MQTT_CONNECTING;
			else {
				mqtt_tls_cleanup(m);
				m->state = MQTT_RECONNECT_WAIT;
				m->reconnect_at_ms = now + 30000;
			}
		} else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
			   ret != MBEDTLS_ERR_SSL_WANT_WRITE &&
			   ret != MBEDTLS_ERR_SSL_TIMEOUT) {
			printf("mqtt: TLS 握手失败 (%d)\n", ret);
			mqtt_tls_cleanup(m);
			m->state = MQTT_RECONNECT_WAIT;
			m->reconnect_at_ms = now + 30000;
		} else {
			/* WANT_READ/WRITE/TIMEOUT: 稍息再试 (避免忙等) */
			usleep(2000);
		}
		break;
	}

	case MQTT_CONNECTING:
	case MQTT_CONNECTED: {
		/* 读 (非阻塞直到 WANT_READ) */
		int ret;

		/* 冲刷积压发送 */
		if (m->txlen > 0) {
			ret = mbedtls_ssl_write(&SS.ssl, m->txbuf, m->txlen);
			if (ret > 0) {
				memmove(m->txbuf, m->txbuf + ret,
					m->txlen - ret);
				m->txlen -= ret;
			}
		}

		for (;;) {
			struct pollfd pfd;
			int pr;

			if (m->rxlen >= (int)sizeof(m->rxbuf) - 1) {
				/* 上限保护: 丢弃整包边界外的数据 */
				m->rxlen = 0;
				break;
			}
			/* poll 守卫: 100ms 无数据即退出读循环,
			 * 让下一轮 pump 的写阶段刷出待发报文 */
			pfd.fd = SS.net.fd;
			pfd.events = POLLIN;
			pfd.revents = 0;
			pr = poll(&pfd, 1, 100);
			if (pr <= 0)
				break;		/* 超时/被信号打断 */
			if (pfd.revents & (POLLHUP | POLLERR)) {
				ssl_err = MBEDTLS_ERR_NET_RECV_FAILED;
				goto conn_lost;
			}
			ret = mbedtls_ssl_read(&SS.ssl,
					       m->rxbuf + m->rxlen,
					       sizeof(m->rxbuf) - m->rxlen);
			if (ret > 0) {
				printf("mqtt: rx %d B\n", ret);
				m->rxlen += ret;
				m->last_rx_ms = now;
			} else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
				   ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
				/* 记录未到齐, 下轮 poll 再读 */
				break;
			} else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
				   ret == 0) {
				ssl_err = ret;
				goto conn_lost;
			} else {
				ssl_err = ret;
				goto conn_lost;
			}
		}

		/* 解析完整报文 */
		{
			int used = mqtt_parse(m);

			if (used > 0) {
				memmove(m->rxbuf, m->rxbuf + used,
					m->rxlen - used);
				m->rxlen -= used;
			}
		}

		/* 刷新时间: 读循环+解析可能耗时 100ms+, 且 CONNACK
		 * 解析里 last_rx 用的是新鲜 sys_ms() — 若沿用 pump 开
		 * 始的旧 now 做无符号减法, last_rx > now 会回绕成天文
		 * 数字, 90s 判定瞬间误触发 (实测 last_rx 领先 100ms) */
		now = sys_ms();

		/* KeepAlive: 30s 间隔 PINGREQ */
		if (m->state == MQTT_CONNECTED &&
		    now - m->last_ping_ms > 30000) {
			uint8_t ping[2] = { 0xC0, 0x00 };

			mbedtls_ssl_write(&SS.ssl, ping, 2);
			m->last_ping_ms = now;
		}

		/* 90 秒无收包判定断线 (协议 5.1: KeepAlive 1.5 倍) */
		if (m->state == MQTT_CONNECTED &&
		    now - m->last_rx_ms > 90000) {
			printf("mqtt: 90s 无收包判死 (last_rx=%llums "
			       "now=%llums)\n",
			       (unsigned long long)m->last_rx_ms,
			       (unsigned long long)now);
			goto conn_lost;
		}
		break;
	}

	default:
		break;
	}
	return;

conn_lost:
	printf("mqtt: 连接断开 (ssl_ret=%d, -0x%x)\n", ssl_err, -ssl_err);
	mqtt_tls_cleanup(m);
	m->state = MQTT_RECONNECT_WAIT;
	m->rxlen = m->txlen = 0;
	m->fail_count++;
	{
		int delay = (m->fail_count >= 10) ? 300000 :
			    (m->fail_count >= 5) ? 60000 : 30000;

		m->reconnect_at_ms = now + delay;
	}
	if (m->on_disconnected)
		m->on_disconnected(m->cb_ctx);
}

/* === 初始化 === */

void mqtt_init(struct mqtt *m)
{
	memset(m, 0, sizeof(*m));
	m->state = MQTT_INIT;
	/* 不自动连接: host 填写后 pump 自动发起 (首次延迟 0) */
}

void mqtt_set_handler(struct mqtt *m, mqtt_msg_fn fn, void *ctx)
{
	m->on_msg = fn;
	m->cb_ctx = ctx;
}
