/*
 * include/pal/tlv.h - 协议 V3.0 TLV 编解码 (协议 2.5)
 *
 * 参数区由 TLV 序列组成: Tag(1) + Len(1) + Value(N)。
 * 供 0x04 解析 / 0x8x 组包共用。
 */
#ifndef PAL_TLV_H
#define PAL_TLV_H

#include <stdint.h>
#include <stddef.h>

/* 遍历回调: 返回非 0 终止遍历并返回该值 */
typedef int (*tlv_iter_fn)(void *ctx, uint8_t tag, const uint8_t *val,
			   uint16_t len);

/* 遍历 TLV 序列。返回: 0=全部遍历完, >0=回调的终止返回值, <0=格式错误 */
int tlv_iterate(const uint8_t *buf, uint16_t len, tlv_iter_fn fn, void *ctx);

/* 查找指定 Tag, 找到返回 0 并填充 val/len, 未找到返回 -1 */
int tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag,
	     const uint8_t **val, uint16_t *out_len);

/* 查找带子类型的 Tag (Value 首字节为子类型), 匹配 subtype 返回 0 */
int tlv_find_subtype(const uint8_t *buf, uint16_t len, uint8_t tag,
		     uint8_t subtype, const uint8_t **val, uint16_t *out_len);

/* === 编码器: 追加式构建参数区 === */

struct tlv_writer {
	uint8_t *buf;
	uint16_t cap;
	uint16_t len;
	int overflow;
};

void tlv_writer_init(struct tlv_writer *w, uint8_t *buf, uint16_t cap);

/* 追加一个 TLV。成功返回 0, 缓冲不足返回 -1 (置 overflow) */
int tlv_add(struct tlv_writer *w, uint8_t tag, const void *val, uint16_t len);

/* 便利: ASCII 字符串 / 单字节 / 双字节大端 / 4/8字节大端 / 原始32字节 */
int tlv_add_str(struct tlv_writer *w, uint8_t tag, const char *s);
int tlv_add_u8(struct tlv_writer *w, uint8_t tag, uint8_t v);
int tlv_add_u16be(struct tlv_writer *w, uint8_t tag, uint16_t v);
int tlv_add_u32be(struct tlv_writer *w, uint8_t tag, uint32_t v);
int tlv_add_u64be(struct tlv_writer *w, uint8_t tag, uint64_t v);

#endif /* PAL_TLV_H */
