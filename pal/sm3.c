/*
 * pal/sm3.c - SM3 密码杂凑算法实现 (GB/T 32905-2016)
 *
 * 标准参考: 国家标准 GB/T 32905-2016
 * 实现为公开算法的直译, 无外部依赖。
 * 自测向量见 app/main.c --selftest。
 */
#include <pal/sm3.h>

#include <string.h>
#include <stdio.h>

#define ROTL(x, n)	(((x) << (n)) | ((x) >> (32 - (n))))
#define P0(x)		((x) ^ ROTL((x), 9) ^ ROTL((x), 17))
#define P1(x)		((x) ^ ROTL((x), 15) ^ ROTL((x), 23))

static const uint32_t sm3_iv[8] = {
	0x7380166f, 0x4914b2b9, 0x172442d7, 0xda8a0600,
	0xa96f30bc, 0x163138aa, 0xe38dee4d, 0xb0fb0e4e,
};

static void sm3_be32enc(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t sm3_be32dec(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 消息扩展 + 压缩函数 (一个 512bit 块) */
static void sm3_compress(uint32_t state[8], const uint8_t block[64])
{
	uint32_t w[68], w1[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t ss1, ss2, tt1, tt2, t;
	int j;

	for (j = 0; j < 16; j++)
		w[j] = sm3_be32dec(block + j * 4);
	for (j = 16; j < 68; j++)
		w[j] = P1(w[j - 16] ^ w[j - 9] ^ ROTL(w[j - 3], 15))
		     ^ ROTL(w[j - 13], 7) ^ w[j - 6];
	for (j = 0; j < 64; j++)
		w1[j] = w[j] ^ w[j + 4];

	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	for (j = 0; j < 64; j++) {
		if (j < 16)
			t = 0x79cc4519u;
		else
			t = 0x7a879d8au;

		ss1 = ROTL(ROTL(a, 12) + e + ROTL(t, j & 31), 7);
		ss2 = ss1 ^ ROTL(a, 12);

		if (j < 16) {
			tt1 = (a ^ b ^ c) + d + ss2 + w1[j];
			tt2 = (e ^ f ^ g) + h + ss1 + w[j];
		} else {
			tt1 = ((a & b) | (a & c) | (b & c)) + d + ss2 + w1[j];
			tt2 = ((e & f) | ((~e) & g)) + h + ss1 + w[j];
		}
		d = c;
		c = ROTL(b, 9);
		b = a;
		a = tt1;
		h = g;
		g = ROTL(f, 19);
		f = e;
		e = P0(tt2);
	}

	state[0] ^= a; state[1] ^= b; state[2] ^= c; state[3] ^= d;
	state[4] ^= e; state[5] ^= f; state[6] ^= g; state[7] ^= h;
}

void sm3_init(struct sm3_ctx *ctx)
{
	memcpy(ctx->state, sm3_iv, sizeof(sm3_iv));
	ctx->total_len = 0;
	ctx->buf_len = 0;
}

void sm3_update(struct sm3_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = data;

	ctx->total_len += len;

	if (ctx->buf_len) {
		size_t fill = SM3_BLOCK_SIZE - ctx->buf_len;

		if (len < fill) {
			memcpy(ctx->buf + ctx->buf_len, p, len);
			ctx->buf_len += len;
			return;
		}
		memcpy(ctx->buf + ctx->buf_len, p, fill);
		sm3_compress(ctx->state, ctx->buf);
		p += fill;
		len -= fill;
		ctx->buf_len = 0;
	}

	while (len >= SM3_BLOCK_SIZE) {
		sm3_compress(ctx->state, p);
		p += SM3_BLOCK_SIZE;
		len -= SM3_BLOCK_SIZE;
	}

	if (len) {
		memcpy(ctx->buf, p, len);
		ctx->buf_len = len;
	}
}

void sm3_final(struct sm3_ctx *ctx, uint8_t digest[SM3_DIGEST_SIZE])
{
	uint8_t pad[72];			/* 0x80 + 0x00*n + 8字节长度 */
	uint64_t bit_len = ctx->total_len * 8;
	size_t pad_len;
	int i;

	pad_len = (ctx->buf_len < 56) ? (56 - ctx->buf_len)
				      : (120 - ctx->buf_len);
	pad[0] = 0x80;
	memset(pad + 1, 0, pad_len - 1);
	for (i = 0; i < 8; i++)
		pad[pad_len + i] = (uint8_t)(bit_len >> (56 - 8 * i));

	sm3_update(ctx, pad, pad_len + 8);
	/* 此时 buf_len 必为 0 */

	for (i = 0; i < 8; i++)
		sm3_be32enc(digest + i * 4, ctx->state[i]);
}

void sm3_buf(const void *data, size_t len, uint8_t digest[SM3_DIGEST_SIZE])
{
	struct sm3_ctx ctx;

	sm3_init(&ctx);
	sm3_update(&ctx, data, len);
	sm3_final(&ctx, digest);
}

int sm3_file(const char *path, uint8_t digest[SM3_DIGEST_SIZE])
{
	struct sm3_ctx ctx;
	FILE *fp;
	uint8_t buf[8192];
	size_t n;

	fp = fopen(path, "rb");
	if (!fp)
		return -1;

	sm3_init(&ctx);
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		sm3_update(&ctx, buf, n);
	fclose(fp);

	sm3_final(&ctx, digest);
	return 0;
}
