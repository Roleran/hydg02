/*
 * include/net/https.h - 极简 HTTPS GET 客户端 (协议 V3.0 8.1)
 *
 * 用于固件下载 (0x04 Tag 0x10 URL) 与接入信息获取 (4.1)。
 * 证书链校验必须开启, 禁止降级明文。
 *
 * 一次性同步接口 (在线程/子进程中调用, 或接受短阻塞):
 * 下载走回调推进度, 便于 0x84 上报。
 */
#ifndef NET_HTTPS_H
#define NET_HTTPS_H

#include <stdint.h>
#include <stddef.h>

/* 进度回调: recv_bytes/total_bytes (total 未知时为 0)。
 * 返回非 0 中止下载 (错误码由调用方定义)。 */
typedef int (*https_progress_fn)(void *ctx, uint64_t recv, uint64_t total);

/* GET 下载到文件。
 * 返回: HTTP 状态码 (200 成功), <0 传输/TLS 错误。
 * md5/sm3 校验由调用方对下载后的文件执行。 */
int https_get_file(const char *url, const char *save_path,
		   const char *ca_cert_path,
		   https_progress_fn progress, void *pctx);

/* GET 到内存缓冲 (小响应: 接入信息 JSON)。
 * 返回: HTTP 状态码, body 写入 buf (NUL 结尾), body_len 输出长度。
 * <0 传输错误。 */
int https_get_mem(const char *url, const char *ca_cert_path,
		  char *buf, int buf_cap, int *body_len);

#endif /* NET_HTTPS_H */
