#include "n32_frame.h"

uint16_t n32_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFFu;
    int i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++) {
            if ((crc & 0x0001u) != 0u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

int n32_frame_build(uint8_t *out, int cap, uint8_t cmd, uint8_t seq,
                    const uint8_t *data, int len)
{
    uint16_t crc;
    int total;
    int i;

    if (len < 0 || len > N32_DATA_MAX) {
        return -1;
    }
    total = 9 + len;
    if (out == NULL || cap < total) {
        return -1;
    }

    out[0] = 0xA5u;
    out[1] = 0x5Au;
    out[2] = N32_VER;
    out[3] = cmd;
    out[4] = seq;
    out[5] = (uint8_t)(len & 0xFF);
    out[6] = (uint8_t)((len >> 8) & 0xFF);
    for (i = 0; i < len; i++) {
        out[7 + i] = data[i];
    }
    crc = n32_crc16(out + 2, 5 + len);
    out[7 + len] = (uint8_t)(crc & 0xFF);
    out[8 + len] = (uint8_t)((crc >> 8) & 0xFF);
    return total;
}

int n32_frame_parse(const uint8_t *buf, int len, struct n32_frame *frame, int *used)
{
    int i;
    int data_len;
    int total;
    uint16_t crc;
    uint16_t got;

    *used = 0;
    if (buf == NULL || len <= 0) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (buf[i] == 0xA5u) {
            break;
        }
    }
    if (i > 0) {
        *used = i;
        return -1;
    }
    if (len < 2) {
        return 0;
    }
    if (buf[1] != 0x5Au) {
        *used = 1;
        return -1;
    }
    if (len < 7) {
        return 0;
    }

    data_len = buf[5] | (buf[6] << 8);
    if (data_len > N32_DATA_MAX) {
        *used = 2;
        return -1;
    }
    total = 9 + data_len;
    if (len < total) {
        return 0;
    }

    crc = n32_crc16(buf + 2, 5 + data_len);
    got = (uint16_t)(buf[7 + data_len] | (buf[8 + data_len] << 8));
    if (crc != got) {
        *used = 2;
        return -1;
    }

    frame->cmd = buf[3];
    frame->seq = buf[4];
    frame->len = data_len;
    for (i = 0; i < data_len; i++) {
        frame->data[i] = buf[7 + i];
    }
    *used = total;
    return 1;
}
