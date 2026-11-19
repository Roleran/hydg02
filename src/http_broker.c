#include "http_broker.h"

#include "md5.h"

#include <stdio.h>
#include <string.h>

static int copy_cap(char *dst, int cap, const char *src, int n)
{
    int i;

    if (n <= 0 || n >= cap) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
    return 0;
}

static int host_char(char c)
{
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= '0' && c <= '9') {
        return 1;
    }
    return c == '.' || c == '-';
}

int broker_split_url(const char *url, char *host, int host_cap,
                     int *port, char *path, int path_cap)
{
    const char *p;
    const char *slash;
    const char *colon;
    int host_len;
    int path_len;

    if (url == NULL || host == NULL || port == NULL || path == NULL) {
        return -1;
    }
    if (strncmp(url, "http://", 7) != 0) {
        return -1;
    }
    p = url + 7;
    if (*p == '\0' || *p == '/' || *p == ':') {
        return -1;
    }
    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (colon != NULL && (slash == NULL || colon < slash)) {
        host_len = (int)(colon - p);
        *port = 0;
        colon++;
        if (*colon < '0' || *colon > '9') {
            return -1;
        }
        while (*colon >= '0' && *colon <= '9') {
            *port = *port * 10 + (*colon - '0');
            if (*port > 65535) {
                return -1;
            }
            colon++;
        }
        if (*port == 0 || (slash == NULL && *colon != '\0') ||
            (slash != NULL && colon != slash)) {
            return -1;
        }
    } else {
        host_len = slash == NULL ? (int)strlen(p) : (int)(slash - p);
        *port = 80;
    }
    if (copy_cap(host, host_cap, p, host_len) != 0) {
        return -1;
    }
    {
        int i;

        for (i = 0; i < host_len; i++) {
            if (!host_char(host[i])) {
                return -1;
            }
        }
    }
    if (slash == NULL) {
        if (path_cap < 2) {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
        return 0;
    }
    path_len = (int)strlen(slash);
    return copy_cap(path, path_cap, slash, path_len);
}

int broker_parse_endpoint(const char *line, char *host, int host_cap, int *port)
{
    const char *p;
    const char *colon;
    int i;
    int n;

    if (line == NULL || host == NULL || port == NULL) {
        return -1;
    }
    p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    colon = strchr(p, ':');
    if (colon == NULL || colon == p) {
        return -1;
    }
    n = (int)(colon - p);
    if (copy_cap(host, host_cap, p, n) != 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (!host_char(host[i])) {
            return -1;
        }
    }
    p = colon + 1;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    *port = 0;
    while (*p >= '0' && *p <= '9') {
        *port = *port * 10 + (*p - '0');
        if (*port > 65535) {
            return -1;
        }
        p++;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*port == 0 || *p != '\0') {
        return -1;
    }
    return 0;
}

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int tcp_connect(const char *host, int port)
{
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *it;
    int fd;
    int rc;

    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, port_text, &hints, &res);
    if (rc != 0) {
        return -1;
    }
    fd = -1;
    for (it = res; it != NULL; it = it->ai_next) {
        struct pollfd pfd;
        int err;
        socklen_t err_len;

        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (set_nonblock(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc == 0) {
            break;
        }
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, 10000) <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        err = 0;
        err_len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int write_all(int fd, const char *buf, int len)
{
    int off;

    off = 0;
    while (off < len) {
        struct pollfd pfd;
        int n;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, 5000) <= 0) {
            return -1;
        }
        n = (int)write(fd, buf + off, (size_t)(len - off));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        off += n;
    }
    return 0;
}

static int read_response(int fd, char *buf, int cap)
{
    int used;
    int left;

    used = 0;
    left = 10000;
    while (left > 0 && used < cap - 1) {
        struct pollfd pfd;
        int n;
        int step = left > 500 ? 500 : left;

        pfd.fd = fd;
        pfd.events = POLLIN;
        n = poll(&pfd, 1, step);
        left -= step;
        if (n == 0) {
            continue;
        }
        if (n < 0) {
            return -1;
        }
        n = (int)read(fd, buf + used, (size_t)(cap - 1 - used));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        used += n;
    }
    buf[used] = '\0';
    return used > 0 ? 0 : -1;
}

static int http_get(const char *url, char *body, int body_cap)
{
    char host[128];
    char path[192];
    char req[512];
    char raw[2048];
    int port;
    int fd;
    int n;
    char *hdr_end;
    char *status;
    const char *text;
    int i;

    if (broker_split_url(url, host, (int)sizeof(host), &port, path, (int)sizeof(path)) != 0) {
        return -1;
    }
    if (port == 80) {
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     path, host);
    } else {
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                     path, host, port);
    }
    if (n <= 0 || n >= (int)sizeof(req)) {
        return -1;
    }
    fd = tcp_connect(host, port);
    if (fd < 0) {
        return -1;
    }
    if (write_all(fd, req, n) != 0) {
        close(fd);
        return -1;
    }
    if (read_response(fd, raw, (int)sizeof(raw)) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    status = raw;
    if (strncmp(status, "HTTP/1.", 7) != 0 || status[7] == '\0' || status[8] != ' ') {
        return -1;
    }
    if (strncmp(status + 9, "200", 3) != 0) {
        return -1;
    }
    hdr_end = strstr(raw, "\r\n\r\n");
    if (hdr_end == NULL) {
        return -1;
    }
    text = hdr_end + 4;
    i = 0;
    while (text[i] != '\0' && text[i] != '\r' && text[i] != '\n' && i < body_cap - 1) {
        body[i] = text[i];
        i++;
    }
    if (i == 0) {
        return -1;
    }
    body[i] = '\0';
    return 0;
}

static int is_content_length(const char *p)
{
    static const char name[] = "content-length:";
    int i;

    for (i = 0; name[i] != '\0'; i++) {
        char c = p[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return 0;
        }
    }
    return 1;
}

static int header_length(const char *hdr, int *out)
{
    const char *p;
    int n;

    p = hdr;
    while (*p != '\0') {
        if (is_content_length(p)) {
            p += 15;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p < '0' || *p > '9') {
                return -1;
            }
            n = 0;
            while (*p >= '0' && *p <= '9') {
                if (n > 100000000) {
                    return -1;
                }
                n = n * 10 + (*p - '0');
                p++;
            }
            *out = n;
            return 0;
        }
        p = strchr(p, '\n');
        if (p == NULL) {
            break;
        }
        p++;
    }
    *out = -1;
    return 0;
}

static int read_chunk(int fd, char *buf, int cap, int *closed,
                      int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    int quiet;

    *closed = 0;
    quiet = 0;
    while (quiet < 30) {
        struct pollfd pfd;
        int n;

        pfd.fd = fd;
        pfd.events = POLLIN;
        n = poll(&pfd, 1, 1000);
        if (pump != NULL && pump(ctx, n == 0 ? 1000 : 0) != 0) {
            return -1;
        }
        if (n == 0) {
            quiet++;
            continue;
        }
        if (n < 0) {
            return -1;
        }
        n = (int)read(fd, buf, (size_t)cap);
        if (n == 0) {
            *closed = 1;
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        return n;
    }
    return -1;
}

int http_save(const char *url, const char *path, uint8_t digest[16], int max_bytes,
              int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    char host[128];
    char req_path[192];
    char req[512];
    char hdr[2048];
    char chunk[1024];
    int port;
    int fd;
    int n;
    int used;
    int closed;
    int content_length;
    int total;
    char *hdr_end;
    FILE *fp;
    struct md5_ctx md;

    if (url == NULL || path == NULL || digest == NULL || max_bytes <= 0) {
        return -1;
    }
    if (broker_split_url(url, host, (int)sizeof(host), &port,
                         req_path, (int)sizeof(req_path)) != 0) {
        return -1;
    }
    if (port == 80) {
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     req_path, host);
    } else {
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                     req_path, host, port);
    }
    if (n <= 0 || n >= (int)sizeof(req)) {
        return -1;
    }
    fd = tcp_connect(host, port);
    if (fd < 0) {
        return -1;
    }
    if (write_all(fd, req, n) != 0) {
        close(fd);
        return -1;
    }

    used = 0;
    hdr_end = NULL;
    while (hdr_end == NULL) {
        if (used >= (int)sizeof(hdr) - 1) {
            close(fd);
            return -1;
        }
        n = read_chunk(fd, hdr + used, (int)sizeof(hdr) - 1 - used, &closed, pump, ctx);
        if (n < 0 || closed) {
            close(fd);
            return -1;
        }
        used += n;
        hdr[used] = '\0';
        hdr_end = strstr(hdr, "\r\n\r\n");
    }
    if (strncmp(hdr, "HTTP/1.", 7) != 0 || hdr[8] != ' ' || strncmp(hdr + 9, "200", 3) != 0) {
        close(fd);
        return -1;
    }
    hdr_end[0] = '\0';
    if (header_length(hdr, &content_length) != 0) {
        close(fd);
        return -1;
    }
    if (content_length > max_bytes) {
        close(fd);
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        close(fd);
        return -1;
    }
    md5_init(&md);
    total = used - (int)(hdr_end + 4 - hdr);
    if (total > 0) {
        if ((content_length >= 0 && total > content_length) || total > max_bytes) {
            fclose(fp);
            close(fd);
            return -1;
        }
        if (fwrite(hdr_end + 4, 1, (size_t)total, fp) != (size_t)total) {
            fclose(fp);
            close(fd);
            return -1;
        }
        md5_update(&md, hdr_end + 4, total);
    } else {
        total = 0;
    }

    while (content_length < 0 || total < content_length) {
        int need = (int)sizeof(chunk);

        if (content_length >= 0 && content_length - total < need) {
            need = content_length - total;
        }
        n = read_chunk(fd, chunk, need, &closed, pump, ctx);
        if (n < 0) {
            fclose(fp);
            close(fd);
            return -1;
        }
        if (closed) {
            break;
        }
        if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
            fclose(fp);
            close(fd);
            return -1;
        }
        md5_update(&md, chunk, n);
        total += n;
        if (total > max_bytes) {
            fclose(fp);
            close(fd);
            return -1;
        }
    }
    close(fd);
    if (fclose(fp) != 0) {
        return -1;
    }
    if (content_length >= 0 && total != content_length) {
        return -1;
    }
    if (total <= 0) {
        return -1;
    }
    md5_final(&md, digest);
    return 0;
}

#endif

static void copy_text(char *dst, int cap, const char *src)
{
    int i;

    for (i = 0; i < cap - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int broker_resolve(const struct app_config *cfg, char *host, int host_cap, int *port)
{
    char line[128];

    if (cfg == NULL || host == NULL || port == NULL || host_cap < 2) {
        return -1;
    }
    if (cfg->http_url[0] == '\0') {
        if (cfg->mqtt_host[0] == '\0' || cfg->mqtt_port <= 0 || cfg->mqtt_port > 65535) {
            return -1;
        }
        copy_text(host, host_cap, cfg->mqtt_host);
        *port = cfg->mqtt_port;
        return 0;
    }
#ifdef _WIN32
    (void)line;
    return -1;
#else
    if (http_get(cfg->http_url, line, (int)sizeof(line)) != 0) {
        return -1;
    }
    return broker_parse_endpoint(line, host, host_cap, port);
#endif
}
