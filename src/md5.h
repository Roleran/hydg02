#ifndef MD5_H
#define MD5_H

#include <stdint.h>

struct md5_ctx {
    uint32_t st[4];
    uint64_t nbits;
    uint8_t buf[64];
    int n;
};

void md5_init(struct md5_ctx *ctx);
void md5_update(struct md5_ctx *ctx, const void *data, int len);
void md5_final(struct md5_ctx *ctx, uint8_t out[16]);

/* 一次性计算。失败返回 -1。 */
int md5_of(const void *data, int len, uint8_t out[16]);

#endif
