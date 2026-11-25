/*
 * net/https.c - 极简 HTTPS GET 客户端 (mbedtls, 证书链强制校验)
 *
 * 支持范围 (协议场景内够用):
 *   - http:// 与 https:// (后者必须校验证书链)
 *   - 响应头跳过, 直接取 body (文件下载 / 小 JSON)
 *   - Content-Length 进度与总量校验
 *   - 不支持 chunked/重定向 (平台下载地址为直链)
 */
#include <net/https.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>

struct https_ctx {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_entropy_context entropy;
	int tls;			/* 1=https */
};

/* 解析 URL: [http|https]://host[:port]/path */
static int parse_url(const char *url, char *scheme, char *host, int host_cap,
		     int *port, char *path, int path_cap)
{
	const char *p, *host_start, *host_end;
	int len, port_def;

	if (strncmp(url, "https://", 8) == 0) {
		*scheme = 's';
		port_def = 443;
		p = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		*scheme = 'h';
		port_def = 80;
		p = url + 7;
	} else {
		return -1;
	}

	host_start = p;
	host_end = strchr(p, '/');
	if (!host_end)
		host_end = p + strlen(p);
	len = host_end - host_start;
	if (len <= 0 || len >= host_cap)
		return -1;

	*port = port_def;
	{
		const char *colon = memchr(host_start, ':', len);

		if (colon) {
			*port = atoi(colon + 1);
			len = colon - host_start;
			if (len <= 0 || *port <= 0)
				return -1;
		}
	}

	memcpy(host, host_start, len);
	host[len] = '\0';

	if (*host_end == '/') {
		strncpy(path, host_end, path_cap - 1);
		path[path_cap - 1] = '\0';
	} else {
		strcpy(path, "/");
	}
	return 0;
}

static void https_ctx_init(struct https_ctx *c)
{
	memset(c, 0, sizeof(*c));
	mbedtls_net_init(&c->net);
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_x509_crt_init(&c->ca);
	mbedtls_ctr_drbg_init(&c->ctr_drbg);
	mbedtls_entropy_init(&c->entropy);
}

static void https_ctx_free(struct https_ctx *c)
{
	mbedtls_ssl_close_notify(&c->ssl);
	mbedtls_net_free(&c->net);
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	mbedtls_x509_crt_free(&c->ca);
	mbedtls_ctr_drbg_free(&c->ctr_drbg);
	mbedtls_entropy_free(&c->entropy);
}

/* 建连 + TLS 握手 + 发 GET。返回 0 成功, <0 失败。 */
static int https_request(struct https_ctx *c, const char *url,
			 const char *ca_cert_path)
{
	char scheme, host[128], path[256], portstr[8];
	const char *pers = "hydg02-https";
	char req[512];
	int port, ret, reqlen, off;

	if (parse_url(url, &scheme, host, sizeof(host), &port, path,
		      sizeof(path)) != 0)
		return -1;
	c->tls = (scheme == 's');
	snprintf(portstr, sizeof(portstr), "%d", port);

	if (mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
				  &c->entropy,
				  (const unsigned char *)pers, strlen(pers)) != 0)
		return -1;

	if (mbedtls_ssl_config_defaults(&c->conf,
					MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		return -1;

	if (c->tls) {
		if (!ca_cert_path || !ca_cert_path[0])
			return -1;	/* 协议: 禁止不校验 */
		if (mbedtls_x509_crt_parse_file(&c->ca, ca_cert_path) != 0)
			return -1;
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
		mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
	} else {
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
	}

	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

	if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0)
		return -1;
	mbedtls_ssl_set_hostname(&c->ssl, host);

	if (mbedtls_net_connect(&c->net, host, portstr,
				MBEDTLS_NET_PROTO_TCP) != 0)
		return -2;

	mbedtls_ssl_set_bio(&c->ssl, &c->net,
			    mbedtls_net_send, mbedtls_net_recv, NULL);

	if (c->tls) {
		while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
			if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
			    ret != MBEDTLS_ERR_SSL_WANT_WRITE)
				return -3;	/* 握手/证书校验失败 */
		}
	}

	reqlen = snprintf(req, sizeof(req),
			  "GET %s HTTP/1.0\r\n"
			  "Host: %s\r\n"
			  "User-Agent: hydg02/1.0\r\n"
			  "Accept: */*\r\n"
			  "\r\n", path, host);

	off = 0;
	while (off < reqlen) {
		ret = mbedtls_ssl_write(&c->ssl, (unsigned char *)req + off,
					reqlen - off);
		if (ret > 0)
			off += ret;
		else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
			 ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			return -4;
	}
	return 0;
}

/* 读响应头。
 * 输出: *status_code, *content_length, *body_off (buf 内 body 起点),
 * *total_read (buf 内已读总字节数, 含 body 前缀)。
 * 返回 0 成功, <0 失败。 */
static int https_read_header(struct https_ctx *c, uint8_t *buf, int cap,
			     int *status_code, int *content_length,
			     int *body_off, int *total_read)
{
	int total = 0, i, header_end = -1;
	char *sc, *cl;

	while (total < cap) {
		int ret = mbedtls_ssl_read(&c->ssl, buf + total, cap - total);

		if (ret > 0) {
			total += ret;
			/* i <= total: 无 body 响应的 \r\n\r\n 恰好结束于
			 * 缓冲末尾 (如 404), 差一会漏判 */
			for (i = 3; i <= total && i < cap; i++) {
				if (buf[i - 1] == '\n' && buf[i - 3] == '\n' &&
				    buf[i - 2] == '\r' && buf[i - 4] == '\r') {
					header_end = i;
					break;
				}
			}
			if (header_end > 0)
				break;
		} else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			   ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue;
		} else {
			return -1;
		}
	}

	if (header_end < 0)
		return -1;

	buf[header_end] = '\0';			/* 临时截断解析头 */
	sc = strstr((char *)buf, "HTTP/1.");
	*status_code = sc ? atoi(sc + 9) : 0;
	cl = strstr((char *)buf, "Content-Length:");
	*content_length = cl ? atoi(cl + 15) : 0;
	buf[header_end] = '\r';			/* 恢复 */

	*body_off = header_end;
	*total_read = total;
	return 0;
}

int https_get_file(const char *url, const char *save_path,
		   const char *ca_cert_path,
		   https_progress_fn progress, void *pctx)
{
	struct https_ctx c;
	static uint8_t buf[8192];
	int fd, status = 0, clen = 0, body_off = 0, got = 0, total = 0, ret;

	https_ctx_init(&c);
	ret = https_request(&c, url, ca_cert_path);
	if (ret != 0)
		goto fail;

	ret = https_read_header(&c, buf, sizeof(buf), &status, &clen,
				&body_off, &got);
	if (ret != 0 || status != 200) {
		ret = status ? status : -5;
		goto fail;
	}

	fd = open(save_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		ret = -6;
		goto fail;
	}

	/* header 读取时捎带的 body 前缀 */
	if (got > body_off) {
		int n = got - body_off;

		if (write(fd, buf + body_off, n) != n) {
			close(fd);
			ret = -7;
			goto fail;
		}
		total += n;
	}

	/* 继续读 body */
	for (;;) {
		ret = mbedtls_ssl_read(&c.ssl, buf, sizeof(buf));
		if (ret > 0) {
			if (write(fd, buf, ret) != ret) {
				close(fd);
				ret = -7;
				goto fail;
			}
			total += ret;
			if (progress && progress(pctx, total, clen) != 0) {
				close(fd);
				ret = -8;
				goto fail;
			}
			if (clen && total >= clen)
				break;
		} else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			   ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue;
		} else {
			break;		/* 对端关闭 = 结束 */
		}
	}

	close(fd);
	https_ctx_free(&c);

	if (clen && total != clen)
		return -9;		/* 长度不符 */

	return 200;

fail:
	https_ctx_free(&c);
	return ret;
}

int https_get_mem(const char *url, const char *ca_cert_path,
		  char *buf, int buf_cap, int *body_len)
{
	struct https_ctx c;
	static uint8_t rbuf[4096];
	int status = 0, clen = 0, body_off = 0, got = 0, total = 0, ret;

	https_ctx_init(&c);
	ret = https_request(&c, url, ca_cert_path);
	if (ret != 0)
		goto fail;

	ret = https_read_header(&c, rbuf, sizeof(rbuf), &status, &clen,
				&body_off, &got);
	if (ret != 0 || status != 200) {
		ret = status ? status : -5;
		goto fail;
	}

	/* header 捎带的 body 前缀 */
	if (got > body_off) {
		int n = got - body_off;

		if (n > buf_cap - 1)
			n = buf_cap - 1;
		memcpy(buf, rbuf + body_off, n);
		total = n;
	}

	/* 继续读 */
	while (total < buf_cap - 1) {
		ret = mbedtls_ssl_read(&c.ssl,
				       (unsigned char *)buf + total,
				       buf_cap - 1 - total);
		if (ret > 0) {
			total += ret;
			if (clen && total >= clen)
				break;
		} else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			   ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue;
		} else {
			break;
		}
	}

	buf[total] = '\0';
	*body_len = total;
	https_ctx_free(&c);
	return 200;

fail:
	https_ctx_free(&c);
	return ret;
}
