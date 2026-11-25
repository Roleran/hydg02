/*
 * common/modbus/modbus_rt.c - Modbus RTU 协议编解码 (公共模块)
 *
 * 标准 Modbus RTU: CRC-16 (poly 0x8005 反射为 0xA001, init 0xFFFF)
 * 帧格式: [addr][func][data...][crc_l][crc_h]  (CRC 低字节在前)
 */

#include <common/modbus_rt.h>

#include <string.h>

/* === CRC-16 Modbus === */

uint16_t modbus_crc16(const uint8_t *data, int len)
{
	uint16_t crc = 0xFFFF;
	int i, j;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (j = 0; j < 8; j++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0xA001;
			else
				crc >>= 1;
		}
	}
	return crc;
}

/* 内部: 附加 CRC 到帧尾 (低字节在前) */
static int append_crc(uint8_t *out, int body_len)
{
	uint16_t crc = modbus_crc16(out, body_len);

	out[body_len] = (uint8_t)(crc & 0xFF);
	out[body_len + 1] = (uint8_t)(crc >> 8);
	return body_len + 2;
}

/* === 主站请求构建 === */

int modbus_build_read(uint8_t addr, uint8_t func,
		      uint16_t start, uint16_t qty,
		      uint8_t *out, int cap)
{
	if (cap < 8)
		return -1;

	out[0] = addr;
	out[1] = func;
	out[2] = (uint8_t)(start >> 8);
	out[3] = (uint8_t)(start & 0xFF);
	out[4] = (uint8_t)(qty >> 8);
	out[5] = (uint8_t)(qty & 0xFF);
	return append_crc(out, 6);
}

int modbus_build_write_single(uint8_t addr, uint16_t reg, uint16_t val,
			      uint8_t *out, int cap)
{
	if (cap < 8)
		return -1;

	out[0] = addr;
	out[1] = MODBUS_FC_WRITE_SINGLE;
	out[2] = (uint8_t)(reg >> 8);
	out[3] = (uint8_t)(reg & 0xFF);
	out[4] = (uint8_t)(val >> 8);
	out[5] = (uint8_t)(val & 0xFF);
	return append_crc(out, 6);
}

/* === 从站响应解析 === */

int modbus_parse_resp(const uint8_t *frame, int len,
		      uint8_t addr, uint8_t func,
		      uint16_t *regs, int reg_count, uint8_t *exc)
{
	uint16_t crc;
	int i;

	if (exc)
		*exc = 0;

	/* 最小帧长: addr + func + 异常码 + crc = 5 */
	if (len < 5)
		return MODBUS_ERR_LEN;

	/* 从站地址校验 */
	if (frame[0] != addr)
		return MODBUS_ERR_ADDR;

	/* 异常响应: func | 0x80 */
	if (frame[1] == (func | 0x80)) {
		if (exc)
			*exc = frame[2];
		return MODBUS_ERR_EXCEPTION;
	}

	/* 功能码校验 */
	if (frame[1] != func)
		return MODBUS_ERR_FUNC;

	/* CRC 校验 (不含 CRC 自身) */
	crc = modbus_crc16(frame, len - 2);
	if ((uint8_t)(crc & 0xFF) != frame[len - 2] ||
	    (uint8_t)(crc >> 8) != frame[len - 1])
		return MODBUS_ERR_CRC;

	/* 读响应: [addr][func][byte_count][data...][crc] */
	{
		int byte_count = frame[2];
		int expect = 3 + byte_count + 2;

		if (byte_count < 2 || (byte_count & 1) != 0)
			return MODBUS_ERR_LEN;
		if (len != expect)
			return MODBUS_ERR_LEN;
		if (byte_count / 2 != reg_count)
			return MODBUS_ERR_LEN;

		for (i = 0; i < reg_count; i++) {
			int off = 3 + i * 2;

			regs[i] = ((uint16_t)frame[off] << 8) |
				  frame[off + 1];
		}
	}

	return MODBUS_OK;
}
