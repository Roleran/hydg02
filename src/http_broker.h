#ifndef HTTP_BROKER_H
#define HTTP_BROKER_H

#include "config.h"

#include <stdint.h>

/*
 * 拆 http://主机[:端口]/路径。缺端口时用 80，缺路径时用 /。
 * 不接受 https。成功返回 0。
 */
int broker_split_url(const char *url, char *host, int host_cap,
                     int *port, char *path, int path_cap);

/* 正文一行：主机:端口。主机只允许字母、数字、点和横线。 */
int broker_parse_endpoint(const char *line, char *host, int host_cap, int *port);

/*
 * http_url 为空时用配置里的 mqtt_host 和 mqtt_port。
 * 非空时 GET 该地址，正文第一行必须是主机:端口。失败返回 -1，不退回配置。
 */
int broker_resolve(const struct app_config *cfg, char *host, int host_cap, int *port);

/*
 * 把 HTTP 正文存成文件，并计算 MD5。超过 max_bytes 算失败。
 * pump 在等待和每写一块之后调用；返回非 0 就中止。
 * 成功返回 0。调用方负责删除失败时留下的半截文件。
 */
int http_save(const char *url, const char *path, uint8_t digest[16], int max_bytes,
              int (*pump)(void *ctx, int idle_ms), void *ctx);

#endif
