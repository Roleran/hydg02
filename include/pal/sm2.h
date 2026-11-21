/*
 * include/pal/sm2.h - SM2 椭圆曲线数字签名 (GB/T 32918.2-2016)
 *
 * 曲线: sm2p256v1
 * 约定(与协议 V3.0 对齐, 见 ota 设计文档):
 *   - UserID 固定 "1234567812345678" (GB/T 32918.2 默认值)
 *   - 签名/验签的消息 M = 32 字节 SM3 摘要(文件摘要)
 *     内部再按标准计算 Z_A = SM3(ENTL||ID||a||b||Gx||Gy||x_A||y_A),
 *     e = SM3(Z_A || M)
 *   - 签名值为 64 字节裸格式: r(32, 大端) || s(32, 大端), 非 ASN.1 DER
 *
 * 验签(sm2_verify)用于设备端校验平台固件签名;
 * 签名(sm2_sign)/导出公钥(sm2_pubkey_from_priv)供测试服务器(C++)使用,
 * 设备端不调用。
 */
#ifndef PAL_SM2_H
#define PAL_SM2_H

#include <stdint.h>

#define SM2_PUBKEY_SIZE	64	/* x(32) || y(32), 大端 */
#define SM2_SIG_SIZE	64	/* r(32) || s(32), 大端 */
#define SM2_PRIV_SIZE	32	/* 私钥标量, 大端 */

/* 验签: pubkey_xy = 平台公钥 x||y, digest = 已算好的 32 字节 SM3 摘要,
 * sig_rs = 64 字节裸签名。返回 1=通过, 0=失败(含参数非法/点不在曲线上)。 */
int sm2_verify(const uint8_t pubkey_xy[SM2_PUBKEY_SIZE],
	       const uint8_t digest[32],
	       const uint8_t sig_rs[SM2_SIG_SIZE]);

/* 签名(测试服务器用): 私钥 d + 32 字节摘要 -> 64 字节裸签名。
 * 随机数 k 取自 /dev/urandom。返回 0=成功。 */
int sm2_sign(const uint8_t priv_d[SM2_PRIV_SIZE],
	     const uint8_t digest[32],
	     uint8_t sig_rs[SM2_SIG_SIZE]);

/* 由私钥导出公钥 x||y (服务器生成测试密钥对用)。返回 0=成功。 */
int sm2_pubkey_from_priv(const uint8_t priv_d[SM2_PRIV_SIZE],
			 uint8_t pubkey_xy[SM2_PUBKEY_SIZE]);

#endif /* PAL_SM2_H */
