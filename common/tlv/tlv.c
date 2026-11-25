/*
 * pal/tlv.c - TLV 编解码实现 (协议 V3.0 2.5)
 */
#include <common/tlv.h>

#include <string.h>

int tlv_iterate(const uint8_t *buf, uint16_t len, tlv_iter_fn fn, void *ctx)
{
	uint16_t pos = 0;

	while (pos + 2 <= len) {
		uint8_t tag = buf[pos];
		uint8_t vlen = buf[pos + 1];
		int ret;

		if (pos + 2 + vlen > len)
			return -1;

		ret = fn(ctx, tag, buf + pos + 2, vlen);
		if (ret)
			return ret;

		pos += 2 + vlen;
	}

	/* 尾部残留非完整 TLV 视为格式错误 */
	if (pos != len)
		return -1;

	return 0;
}

int tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag,
	     const uint8_t **val, uint16_t *out_len)
{
	uint16_t pos = 0;

	while (pos + 2 <= len) {
		uint8_t t = buf[pos];
		uint8_t vlen = buf[pos + 1];

		if (pos + 2 + vlen > len)
			return -1;

		if (t == tag) {
			*val = buf + pos + 2;
			*out_len = vlen;
			return 0;
		}
		pos += 2 + vlen;
	}
	return -1;
}

int tlv_find_subtype(const uint8_t *buf, uint16_t len, uint8_t tag,
		     uint8_t subtype, const uint8_t **val, uint16_t *out_len)
{
	uint16_t pos = 0;

	while (pos + 2 <= len) {
		uint8_t t = buf[pos];
		uint8_t vlen = buf[pos + 1];

		if (pos + 2 + vlen > len)
			return -1;

		if (t == tag && vlen >= 1 && buf[pos + 2] == subtype) {
			*val = buf + pos + 3;	/* 跳过子类型字节 */
			*out_len = vlen - 1;
			return 0;
		}
		pos += 2 + vlen;
	}
	return -1;
}

void tlv_writer_init(struct tlv_writer *w, uint8_t *buf, uint16_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->overflow = 0;
}

int tlv_add(struct tlv_writer *w, uint8_t tag, const void *val, uint16_t len)
{
	if (w->overflow)
		return -1;
	if (len > 255 || w->len + 2 + len > w->cap) {
		w->overflow = 1;
		return -1;
	}

	w->buf[w->len++] = tag;
	w->buf[w->len++] = (uint8_t)len;
	memcpy(w->buf + w->len, val, len);
	w->len += len;
	return 0;
}

int tlv_add_str(struct tlv_writer *w, uint8_t tag, const char *s)
{
	size_t len = strlen(s);

	if (len > 255)
		return -1;
	return tlv_add(w, tag, s, (uint16_t)len);
}

int tlv_add_u8(struct tlv_writer *w, uint8_t tag, uint8_t v)
{
	return tlv_add(w, tag, &v, 1);
}

int tlv_add_u16be(struct tlv_writer *w, uint8_t tag, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

	return tlv_add(w, tag, b, 2);
}

int tlv_add_u32be(struct tlv_writer *w, uint8_t tag, uint32_t v)
{
	uint8_t b[4] = {
		(uint8_t)(v >> 24), (uint8_t)(v >> 16),
		(uint8_t)(v >> 8), (uint8_t)v
	};

	return tlv_add(w, tag, b, 4);
}

int tlv_add_u64be(struct tlv_writer *w, uint8_t tag, uint64_t v)
{
	uint8_t b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (56 - 8 * i));

	return tlv_add(w, tag, b, 8);
}
