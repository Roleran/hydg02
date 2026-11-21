/*
 * include/pal/sm3.h - SM3 密码杂凑算法 (GB/T 32905-2016)
 *
 * 流式接口: init → update* → final。
 * 协议 V3.0 用于文件完整性摘要 (0x04 Tag 0x11)。
 */
#ifndef PAL_SM3_H
#define PAL_SM3_H

#include <stdint.h>
#include <stddef.h>

#define SM3_DIGEST_SIZE	32
#define SM3_BLOCK_SIZE	64

struct sm3_ctx {
	uint32_t state[8];
	uint64_t total_len;		/* 已处理字节数 */
	uint8_t  buf[SM3_BLOCK_SIZE];
	size_t   buf_len;
};

void sm3_init(struct sm3_ctx *ctx);
void sm3_update(struct sm3_ctx *ctx, const void *data, size_t len);
void sm3_final(struct sm3_ctx *ctx, uint8_t digest[SM3_DIGEST_SIZE]);

/* 一次性便利接口 */
void sm3_buf(const void *data, size_t len, uint8_t digest[SM3_DIGEST_SIZE]);

/* 文件流式摘要: 成功返回 0 */
int sm3_file(const char *path, uint8_t digest[SM3_DIGEST_SIZE]);

#endif /* PAL_SM3_H */
