/*
 * include/common/modbus_rt.h - Modbus RTU 协议编解码 (公共模块)
 *
 * 纯协议层, 不碰串口 — 传输由业务层 (subsystems/serial) 提供。
 * 标准: Modbus RTU over RS-485, CRC-16 (poly 0x8005, init 0xFFFF)
 *
 * 本设备为 Modbus 主站 (master):
 *   构建请求帧 → 业务层发到通道 → 收响应 → 解析校验
 */

#ifndef COMMON_MODBUS_RT_H
#define COMMON_MODBUS_RT_H

#include <stdint.h>

/* 功能码 */
#define MODBUS_FC_READ_HOLDING		0x03
#define MODBUS_FC_READ_INPUT		0x04
#define MODBUS_FC_WRITE_SINGLE		0x06
#define MODBUS_FC_WRITE_MULTI		0x10

/* modbus_parse_resp 返回码 */
#define MODBUS_OK			0
#define MODBUS_ERR_CRC			-1
#define MODBUS_ERR_EXCEPTION		-2	/* 从站返回异常码, 具体值通过 *exc */
#define MODBUS_ERR_LEN			-3
#define MODBUS_ERR_ADDR			-4
#define MODBUS_ERR_FUNC			-5

/* CRC-16 Modbus (poly 0xA001 反射, init 0xFFFF) */
uint16_t modbus_crc16(const uint8_t *data, int len);

/*
 * 构建主站读寄存器请求帧 (功能码 03/04)
 * 返回帧总长 (含 CRC), 失败返回 -1 (cap 不足)
 * 输出格式: [addr][func][start_h][start_l][qty_h][qty_l][crc_l][crc_h]
 */
int modbus_build_read(uint8_t addr, uint8_t func,
		      uint16_t start, uint16_t qty,
		      uint8_t *out, int cap);

/*
 * 构建主站写单寄存器请求帧 (功能码 06)
 * 输出格式: [addr][06][reg_h][reg_l][val_h][val_l][crc_l][crc_h]
 */
int modbus_build_write_single(uint8_t addr, uint16_t reg, uint16_t val,
			      uint8_t *out, int cap);

/*
 * 解析从站响应帧 (功能码 03/04 的读响应)
 * 校验: CRC / 从站地址 / 功能码 / 长度一致性
 * 提取: N 个寄存器值写入 regs[] (大端字节序转换)
 *
 * 返回: MODBUS_OK / MODBUS_ERR_* (异常时 *exc 为从站异常码)
 *
 * 响应格式: [addr][func][byte_count][data...][crc_l][crc_h]
 * 异常格式: [addr][func|0x80][exc_code][crc_l][crc_h]
 */
int modbus_parse_resp(const uint8_t *frame, int len,
		      uint8_t addr, uint8_t func,
		      uint16_t *regs, int reg_count, uint8_t *exc);

#endif /* COMMON_MODBUS_RT_H */
