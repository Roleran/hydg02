/*
 * pal/sm2.c - SM2 数字签名实现 (GB/T 32918.2-2016, 曲线 sm2p256v1)
 *
 * 域约定(关键):
 *   - Jacobian 点坐标在 mod-p Montgomery 域运算
 *   - 标量乘的标量用普通域位迭代 (Montgomery 表示的位 != 原值位)
 *   - 验签 mod-n 加减用普通域条件减法
 *   - 签名 mod-n 乘法用 Montgomery, 输出前 mont_back
 *
 * 别名安全:
 *   - mont_mul: r 可与 a/b 别名 (结果最后才写出)
 *   - ecp_double: r 可与 a 别名 (全部中间量在局部)
 *   - ecp_add: r 不可与 a/b 别名 (调用方用临时变量)
 */
#include <pal/sm2.h>
#include <pal/sm3.h>

#include <string.h>
#include <stdio.h>

/* === 曲线参数 sm2p256v1 === */

static const uint8_t sm2_p_be[32] = {
	0xFF,0xFF,0xFF,0xFE,0xFF,0xFF,0xFF,0xFF,
	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};
static const uint8_t sm2_a_be[32] = {
	0xFF,0xFF,0xFF,0xFE,0xFF,0xFF,0xFF,0xFF,
	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFC,
};
static const uint8_t sm2_b_be[32] = {
	0x28,0xE9,0xFA,0x9E,0x9D,0x9F,0x5E,0x34,0x4D,0x5A,0x9E,0x4B,0xCF,0x65,0x09,0xA7,
	0xF3,0x97,0x89,0xF5,0x15,0xAB,0x8F,0x92,0xDD,0xBC,0xBD,0x41,0x4D,0x94,0x0E,0x93
};
static const uint8_t sm2_n_be[32] = {
	0xFF,0xFF,0xFF,0xFE,0xFF,0xFF,0xFF,0xFF,
	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	0x72,0x03,0xDF,0x6B,0x21,0xC6,0x05,0x2B,
	0x53,0xBB,0xF4,0x09,0x39,0xD5,0x41,0x23,
};
static const uint8_t sm2_gx_be[32] = {
	0x32,0xC4,0xAE,0x2C,0x1F,0x19,0x81,0x19,0x5F,0x99,0x04,0x46,0x6A,0x39,0xC9,0x94,
	0x8F,0xE3,0x0B,0xBF,0xF2,0x66,0x0B,0xE1,0x71,0x5A,0x45,0x89,0x33,0x4C,0x74,0xC7
};
static const uint8_t sm2_gy_be[32] = {
	0xBC,0x37,0x36,0xA2,0xF4,0xF6,0x77,0x9C,0x59,0xBD,0xCE,0xE3,0x6B,0x69,0x21,0x53,
	0xD0,0xA9,0x87,0x7C,0xC6,0x2A,0x47,0x40,0x02,0xDF,0x32,0xE5,0x21,0x39,0xF0,0xA0
};

/* === 1. 256 位大数基础 (limb 小端) === */

typedef uint32_t bn[8];

static void bn_zero(bn r) { memset(r, 0, sizeof(bn)); }
static void bn_one(bn r) { bn_zero(r); r[0] = 1; }

static void bn_from_be(bn r, const uint8_t in[32])
{
	int i;

	for (i = 0; i < 8; i++) {
		const uint8_t *p = in + (7 - i) * 4;

		r[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	}
}

static void bn_to_be(uint8_t out[32], const bn a)
{
	int i;

	for (i = 0; i < 8; i++) {
		uint8_t *p = out + (7 - i) * 4;

		p[0] = (uint8_t)(a[i] >> 24);
		p[1] = (uint8_t)(a[i] >> 16);
		p[2] = (uint8_t)(a[i] >> 8);
		p[3] = (uint8_t)a[i];
	}
}

static int bn_is_zero(const bn a)
{
	int i;

	for (i = 0; i < 8; i++)
		if (a[i])
			return 0;
	return 1;
}

static int bn_cmp(const bn a, const bn b)
{
	int i;

	for (i = 7; i >= 0; i--) {
		if (a[i] < b[i])
			return -1;
		if (a[i] > b[i])
			return 1;
	}
	return 0;
}

static uint32_t bn_add_raw(bn r, const bn a, const bn b)
{
	uint64_t carry = 0;
	int i;

	for (i = 0; i < 8; i++) {
		carry += (uint64_t)a[i] + b[i];
		r[i] = (uint32_t)carry;
		carry >>= 32;
	}
	return (uint32_t)carry;
}

static uint32_t bn_sub_raw(bn r, const bn a, const bn b)
{
	uint32_t borrow = 0;
	int i;

	for (i = 0; i < 8; i++) {
		uint64_t t = (uint64_t)a[i] - b[i] - borrow;

		r[i] = (uint32_t)t;
		borrow = (t >> 32) ? 1 : 0;
	}
	return borrow;
}

/* r = (a + b) mod m; a, b < m */
static void bn_add_mod(bn r, const bn a, const bn b, const bn m)
{
	uint32_t carry = bn_add_raw(r, a, b);

	if (carry || bn_cmp(r, m) >= 0)
		bn_sub_raw(r, r, m);
}

/* r = a mod m (a < 2^256) */
static void bn_mod(bn r, const bn a, const bn m)
{
	bn t;

	memcpy(t, a, sizeof(bn));
	while (bn_cmp(t, m) >= 0)
		bn_sub_raw(t, t, m);
	memcpy(r, t, sizeof(bn));
}

/* === 2. Montgomery 模乘 === */

struct mont_ctx {
	bn n;
	uint32_t n0i;		/* -n^-1 mod 2^32 */
	bn r2;			/* R^2 mod n */
	bn one;			/* R mod n (Montgomery 域的 1) */
};

static uint32_t mont_inv32(uint32_t n)
{
	uint32_t x = 1;
	int i;

	for (i = 0; i < 5; i++)
		x *= 2u - n * x;
	return (uint32_t)(-(int32_t)x);
}

static void mont_init(struct mont_ctx *ctx, const uint8_t n_be[32])
{
	bn x;
	int i;

	bn_from_be(ctx->n, n_be);
	ctx->n0i = mont_inv32(ctx->n[0]);

	/* x = R mod n (从 1 翻倍 256 次, 每步条件减) */
	bn_one(x);
	for (i = 0; i < 256; i++) {
		uint32_t carry = 0;
		int j;

		for (j = 0; j < 8; j++) {
			uint64_t t = (uint64_t)x[j] * 2 + carry;

			x[j] = (uint32_t)t;
			carry = (uint32_t)(t >> 32);
		}
		while (carry || bn_cmp(x, ctx->n) >= 0) {
			bn_sub_raw(x, x, ctx->n);
			carry = 0;
		}
	}
	memcpy(ctx->one, x, sizeof(bn));

	/* r2 = R^2 mod n (x = R 再翻倍 256 次) */
	for (i = 0; i < 256; i++) {
		uint32_t carry = 0;
		int j;

		for (j = 0; j < 8; j++) {
			uint64_t t = (uint64_t)x[j] * 2 + carry;

			x[j] = (uint32_t)t;
			carry = (uint32_t)(t >> 32);
		}
		while (carry || bn_cmp(x, ctx->n) >= 0) {
			bn_sub_raw(x, x, ctx->n);
			carry = 0;
		}
	}
	memcpy(ctx->r2, x, sizeof(bn));
}

/* r = a * b * R^-1 mod n (CIOS; r 可与 a/b 别名) */
static void mont_mul(bn r, const bn a, const bn b, const struct mont_ctx *ctx)
{
	uint32_t t[10];
	int i, j;

	memset(t, 0, sizeof(t));

	for (i = 0; i < 8; i++) {
		uint64_t carry = 0;
		uint32_t m;

		for (j = 0; j < 8; j++) {
			uint64_t u = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + carry;

			t[j] = (uint32_t)u;
			carry = u >> 32;
		}
		{
			uint64_t u = (uint64_t)t[8] + carry;

			t[8] = (uint32_t)u;
			t[9] += (uint32_t)(u >> 32);
		}

		m = t[0] * ctx->n0i;
		carry = 0;
		for (j = 0; j < 8; j++) {
			uint64_t u = (uint64_t)t[j] + (uint64_t)m * ctx->n[j] + carry;

			t[j] = (uint32_t)u;
			carry = u >> 32;
		}
		{
			uint64_t u = (uint64_t)t[8] + carry;

			t[8] = (uint32_t)u;
			t[9] += (uint32_t)(u >> 32);
		}

		for (j = 0; j < 9; j++)
			t[j] = t[j + 1];
		t[9] = 0;
	}

	/* 保守最终约减: t 为 9 limb, 逐次条件减到 < n */
	while (t[8]) {
		uint32_t borrow = bn_sub_raw(t, t, ctx->n);

		if (borrow)
			t[8] -= 1;
	}
	while (bn_cmp(t, ctx->n) >= 0)
		bn_sub_raw(t, t, ctx->n);

	memcpy(r, t, sizeof(bn));
}

static void mont_from(bn r, const bn a, const struct mont_ctx *ctx)
{
	mont_mul(r, a, ctx->r2, ctx);
}

static void mont_back(bn r, const bn a, const struct mont_ctx *ctx)
{
	bn one;

	bn_one(one);
	mont_mul(r, a, one, ctx);
}

/* Montgomery 域模加/模减 */
static void mont_add(bn r, const bn a, const bn b, const struct mont_ctx *ctx)
{
	bn_add_mod(r, a, b, ctx->n);
}

static void mont_sub(bn r, const bn a, const bn b, const struct mont_ctx *ctx)
{
	uint32_t borrow = bn_sub_raw(r, a, b);

	if (borrow)
		bn_add_raw(r, r, ctx->n);
	while (bn_cmp(r, ctx->n) >= 0)
		bn_sub_raw(r, r, ctx->n);
}

/* r = a^-1 mod n (Fermat: a^(n-2); a 为 Montgomery 域) */
static void mont_inv(bn r, const bn a, const struct mont_ctx *ctx)
{
	bn e, base, two;
	int i;

	memcpy(base, a, sizeof(bn));
	memcpy(r, ctx->one, sizeof(bn));

	/* e = n - 2 (普通域) */
	bn_one(two);
	two[0] = 2;
	bn_sub_raw(e, ctx->n, two);

	for (i = 255; i >= 0; i--) {
		mont_mul(r, r, r, ctx);
		if ((e[i / 32] >> (i % 32)) & 1)
			mont_mul(r, r, base, ctx);
	}
}

/* === 3. Jacobian 点运算 (mod-p Montgomery 域) === */

struct ecp {
	bn x, y, z;		/* (X:Y:Z) = (X/Z^2, Y/Z^3); Z=0 为无穷远 */
};

static struct mont_ctx mp;	/* mod p */
static struct mont_ctx mn;	/* mod n (签名用) */
static struct ecp G_pt;
static int sm2_inited;

static void ecp_set_inf(struct ecp *p)
{
	bn_zero(p->x);
	bn_zero(p->y);
	bn_zero(p->z);
}

static int ecp_is_inf(const struct ecp *p)
{
	return bn_is_zero(p->z);
}

static void ecp_set_affine(struct ecp *p, const bn x, const bn y)
{
	memcpy(p->x, x, sizeof(bn));
	memcpy(p->y, y, sizeof(bn));
	memcpy(p->z, mp.one, sizeof(bn));	/* Z = 1 */
}

/* 倍点 (a = p-3 专用公式); r 可与 a 别名 */
static void ecp_double(struct ecp *r, const struct ecp *a)
{
	bn xx, yy, zz, m, s, x3, y3, z3, t, t2;

	if (ecp_is_inf(a) || bn_is_zero(a->y)) {
		ecp_set_inf(r);
		return;
	}

	mont_mul(xx, a->x, a->x, &mp);		/* XX = X^2 */
	mont_mul(yy, a->y, a->y, &mp);		/* YY = Y^2 */
	mont_mul(zz, a->z, a->z, &mp);		/* ZZ = Z^2 */

	/* M = 3*(X+ZZ)*(X-ZZ) */
	mont_add(t, a->x, zz, &mp);
	mont_sub(t2, a->x, zz, &mp);
	mont_mul(m, t, t2, &mp);
	mont_add(t, m, m, &mp);		/* t = 2P */
	mont_add(m, m, t, &mp);		/* M = P + 2P = 3P */

	/* S = 4*X*YY */
	mont_mul(s, a->x, yy, &mp);
	mont_add(t, s, s, &mp);
	mont_add(s, t, t, &mp);

	/* X3 = M^2 - 2S */
	mont_mul(x3, m, m, &mp);
	mont_sub(x3, x3, s, &mp);
	mont_sub(x3, x3, s, &mp);

	/* Z3 = 2*Y*Z (先算, 避免别名冲突) */
	mont_mul(z3, a->y, a->z, &mp);
	mont_add(z3, z3, z3, &mp);

	/* Y3 = M*(S - X3) - 8*YY^2 */
	mont_sub(t, s, x3, &mp);
	mont_mul(t, m, t, &mp);
	mont_mul(yy, yy, yy, &mp);		/* YY^2 */
	mont_add(t2, yy, yy, &mp);
	mont_add(yy, t2, t2, &mp);
	mont_add(t2, yy, yy, &mp);		/* 8*YY^2 */
	mont_sub(y3, t, t2, &mp);

	memcpy(r->x, x3, sizeof(bn));
	memcpy(r->y, y3, sizeof(bn));
	memcpy(r->z, z3, sizeof(bn));
}

/* 点加; r 不可与 a/b 别名 */
static void ecp_add(struct ecp *r, const struct ecp *a, const struct ecp *b)
{
	bn z1z1, z2z2, u1, u2, s1, s2, h, h2, h3, rr, t;

	if (ecp_is_inf(a)) {
		*r = *b;
		return;
	}
	if (ecp_is_inf(b)) {
		*r = *a;
		return;
	}

	mont_mul(z1z1, a->z, a->z, &mp);
	mont_mul(z2z2, b->z, b->z, &mp);

	mont_mul(u1, a->x, z2z2, &mp);		/* U1 = X1*Z2^2 */
	mont_mul(u2, b->x, z1z1, &mp);		/* U2 = X2*Z1^2 */

	mont_mul(t, b->z, z2z2, &mp);		/* Z2^3 */
	mont_mul(s1, a->y, t, &mp);
	mont_mul(t, a->z, z1z1, &mp);		/* Z1^3 */
	mont_mul(s2, b->y, t, &mp);

	if (bn_cmp(u1, u2) == 0) {
		if (bn_cmp(s1, s2) != 0)
			ecp_set_inf(r);
		else
			ecp_double(r, a);	/* r 与 a 别名: double 安全 */
		return;
	}

	mont_sub(h, u2, u1, &mp);			/* H = U2 - U1 */
	mont_sub(rr, s2, s1, &mp);			/* R = S2 - S1 */

	mont_mul(h2, h, h, &mp);
	mont_mul(h3, h, h2, &mp);
	mont_mul(t, u1, h2, &mp);			/* U1*H^2 */

	/* X3 = R^2 - H^3 - 2*U1*H^2 */
	mont_mul(r->x, rr, rr, &mp);
	mont_sub(r->x, r->x, h3, &mp);
	mont_sub(r->x, r->x, t, &mp);
	mont_sub(r->x, r->x, t, &mp);

	/* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
	mont_sub(t, t, r->x, &mp);
	mont_mul(r->y, rr, t, &mp);
	mont_mul(t, s1, h3, &mp);
	mont_sub(r->y, r->y, t, &mp);

	/* Z3 = H*Z1*Z2 */
	mont_mul(t, a->z, b->z, &mp);
	mont_mul(r->z, h, t, &mp);
}

/* 标量乘: r = k * a; k 为普通域标量; r 可与 a 别名 */
static void ecp_mul(struct ecp *r, const struct ecp *a, const bn k)
{
	struct ecp res, tmp;
	int i;

	ecp_set_inf(&res);

	for (i = 255; i >= 0; i--) {
		ecp_double(&res, &res);
		if ((k[i / 32] >> (i % 32)) & 1) {
			ecp_add(&tmp, &res, a);
			res = tmp;
		}
	}
	*r = res;
}

/* Jacobian -> 仿射 (输出 Montgomery 域) */
static void ecp_to_affine(bn x, bn y, const struct ecp *p)
{
	bn zi, zi2, zi3;

	if (ecp_is_inf(p)) {
		bn_zero(x);
		bn_zero(y);
		return;
	}

	mont_inv(zi, p->z, &mp);
	mont_mul(zi2, zi, zi, &mp);
	mont_mul(zi3, zi2, zi, &mp);
	mont_mul(x, p->x, zi2, &mp);
	mont_mul(y, p->y, zi3, &mp);
}

/* === 4. Z_A / 验签 / 签名 === */

static const uint8_t sm2_default_uid[] = "1234567812345678";

static void sm2_init_all(void)
{
	bn t;

	if (sm2_inited)
		return;

	mont_init(&mp, sm2_p_be);
	mont_init(&mn, sm2_n_be);

	bn_from_be(t, sm2_gx_be);
	mont_from(G_pt.x, t, &mp);
	bn_from_be(t, sm2_gy_be);
	mont_from(G_pt.y, t, &mp);
	memcpy(G_pt.z, mp.one, sizeof(bn));

	sm2_inited = 1;
}

/* Z_A = SM3(ENTL || ID || a || b || Gx || Gy || xA || yA); xA/yA Montgomery 域 */
static void sm2_compute_za(uint8_t za[32], const bn xa_m, const bn ya_m)
{
	struct sm3_ctx c;
	uint8_t entl[2], buf[32];
	int uid_len = sizeof(sm2_default_uid) - 1;

	entl[0] = (uint8_t)((uid_len * 8) >> 8);	/* 0x00 */
	entl[1] = (uint8_t)(uid_len * 8);		/* 0x80 */

	{
		bn t;

		mont_back(t, xa_m, &mp);
		bn_to_be(buf, t);
		sm3_init(&c);
		sm3_update(&c, entl, 2);
		sm3_update(&c, sm2_default_uid, uid_len);
		sm3_update(&c, sm2_a_be, 32);
		sm3_update(&c, sm2_b_be, 32);
		sm3_update(&c, sm2_gx_be, 32);
		sm3_update(&c, sm2_gy_be, 32);
		sm3_update(&c, buf, 32);
		mont_back(t, ya_m, &mp);
		bn_to_be(buf, t);
		sm3_update(&c, buf, 32);
		sm3_final(&c, za);
	}
}

/* e = SM3(Z_A || M) 转普通域大数 */
static void sm2_compute_e(bn e, const bn xa_m, const bn ya_m,
			  const uint8_t digest[32])
{
	uint8_t za[32], eb[32];
	struct sm3_ctx c;

	sm2_compute_za(za, xa_m, ya_m);
	sm3_init(&c);
	sm3_update(&c, za, 32);
	sm3_update(&c, digest, 32);
	sm3_final(&c, eb);
	bn_from_be(e, eb);
}

/* 公钥点在曲线上 (输入 Montgomery 域) */
static int ecp_on_curve(const bn x, const bn y)
{
	bn lhs, rhs, t;

	mont_mul(lhs, y, y, &mp);

	mont_mul(rhs, x, x, &mp);
	mont_mul(rhs, rhs, x, &mp);			/* x^3 */

	bn_from_be(t, sm2_a_be);
	mont_from(t, t, &mp);
	mont_mul(t, t, x, &mp);
	mont_add(rhs, rhs, t, &mp);			/* + a*x */

	bn_from_be(t, sm2_b_be);
	mont_from(t, t, &mp);
	mont_add(rhs, rhs, t, &mp);			/* + b */

	return bn_cmp(lhs, rhs) == 0;
}

int sm2_verify(const uint8_t pubkey_xy[SM2_PUBKEY_SIZE],
	       const uint8_t digest[32],
	       const uint8_t sig_rs[SM2_SIG_SIZE])
{
	bn r, s, e, t, x1, y1, R, n;
	bn pa_x, pa_y;
	struct ecp pa, p1, p2, sum;

	sm2_init_all();
	bn_from_be(n, sm2_n_be);

	/* 1. 1 <= r, s <= n-1 */
	bn_from_be(r, sig_rs);
	bn_from_be(s, sig_rs + 32);
	{
		bn one;

		bn_one(one);
		if (bn_cmp(r, one) < 0 || bn_cmp(r, n) >= 0)
			return 0;
		if (bn_cmp(s, one) < 0 || bn_cmp(s, n) >= 0)
			return 0;
	}

	/* 2. 公钥入域 + 曲线校验 */
	bn_from_be(pa_x, pubkey_xy);
	bn_from_be(pa_y, pubkey_xy + 32);
	mont_from(pa_x, pa_x, &mp);
	mont_from(pa_y, pa_y, &mp);
	if (!ecp_on_curve(pa_x, pa_y))
		return 0;

	/* 3. e = SM3(Z_A || M) */
	sm2_compute_e(e, pa_x, pa_y, digest);
	bn_mod(e, e, n);

	/* 4. t = (r + s) mod n, t != 0 */
	bn_add_mod(t, r, s, n);
	if (bn_is_zero(t))
		return 0;

	/* 5. (x1, y1) = s*G + t*P_A */
	ecp_set_affine(&pa, pa_x, pa_y);
	ecp_mul(&p1, &G_pt, s);
	ecp_mul(&p2, &pa, t);
	ecp_add(&sum, &p1, &p2);
	if (ecp_is_inf(&sum))
		return 0;
	ecp_to_affine(x1, y1, &sum);

	/* 6. R = (e + x1) mod n, 与 r 比对 */
	mont_back(x1, x1, &mp);
	bn_mod(x1, x1, n);
	bn_add_mod(R, e, x1, n);

	return bn_cmp(R, r) == 0;
}

int sm2_pubkey_from_priv(const uint8_t priv_d[SM2_PRIV_SIZE],
			 uint8_t pubkey_xy[SM2_PUBKEY_SIZE])
{
	bn d, x, y;
	struct ecp pub;

	sm2_init_all();

	bn_from_be(d, priv_d);
	ecp_mul(&pub, &G_pt, d);
	ecp_to_affine(x, y, &pub);
	mont_back(x, x, &mp);
	mont_back(y, y, &mp);
	bn_to_be(pubkey_xy, x);
	bn_to_be(pubkey_xy + 32, y);
	return 0;
}

int sm2_sign(const uint8_t priv_d[SM2_PRIV_SIZE],
	     const uint8_t digest[32],
	     uint8_t sig_rs[SM2_SIG_SIZE])
{
	bn d_plain, d, e, k, x1, y1, r, s, t1;
	bn pub_x, pub_y, n;
	struct ecp pub, kg;
	FILE *fp;
	int retry;

	sm2_init_all();
	bn_from_be(n, sm2_n_be);
	bn_from_be(d_plain, priv_d);

	/* 公钥 (Z_A 需要) */
	ecp_mul(&pub, &G_pt, d_plain);
	ecp_to_affine(pub_x, pub_y, &pub);

	/* e = SM3(Z_A || M) mod n */
	sm2_compute_e(e, pub_x, pub_y, digest);
	bn_mod(e, e, n);

	/* d 进 Montgomery-n 域 */
	bn_mod(d, d_plain, n);
	mont_from(d, d, &mn);

	fp = fopen("/dev/urandom", "rb");
	if (!fp)
		return -1;

	for (retry = 0; retry < 64; retry++) {
		uint8_t kb[32];
		bn k_m, r_m, rd, inv, base;

		if (fread(kb, 1, 32, fp) != 32)
			break;

		bn_from_be(k, kb);
		bn_mod(k, k, n);
		if (bn_is_zero(k))
			continue;

		/* (x1, y1) = k*G */
		ecp_mul(&kg, &G_pt, k);
		ecp_to_affine(x1, y1, &kg);
		mont_back(x1, x1, &mp);
		bn_mod(x1, x1, n);

		/* r = (e + x1) mod n */
		bn_add_mod(r, e, x1, n);
		if (bn_is_zero(r))
			continue;

		/* r + k == n 则重试 */
		bn_add_mod(t1, r, k, n);
		if (bn_is_zero(t1))
			continue;

		/* s = (1+d)^-1 * (k - r*d) mod n (Montgomery 域) */
		mont_from(k_m, k, &mn);
		mont_from(r_m, r, &mn);
		mont_mul(rd, r_m, d, &mn);		/* r*d */
		mont_sub(r_m, k_m, rd, &mn);		/* k - r*d */

		bn_one(base);
		mont_from(base, base, &mn);
		mont_add(base, base, d, &mn);		/* 1 + d */
		mont_inv(inv, base, &mn);		/* (1+d)^-1 */

		mont_mul(s, inv, r_m, &mn);
		mont_back(s, s, &mn);
		if (bn_is_zero(s))
			continue;

		bn_to_be(sig_rs, r);
		bn_to_be(sig_rs + 32, s);
		fclose(fp);
		return 0;
	}

	fclose(fp);
	return -1;
}
