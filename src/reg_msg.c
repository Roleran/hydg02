#include "reg_msg.h"

#include <string.h>

static int put_str(uint8_t *out, int cap, int pos, const char *text)
{
    int n;
    int i;

    if (text == NULL) {
        return -1;
    }
    n = (int)strlen(text);
    if (n <= 0 || n > 255 || pos < 0 || pos + 1 + n > cap) {
        return -1;
    }
    out[pos] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        out[pos + 1 + i] = (uint8_t)text[i];
    }
    return pos + 1 + n;
}

int reg_build(uint8_t *out, int cap, uint64_t seq_ms,
              const char *imei, const char *firmware, const char *iccid)
{
    int pos;
    int i;

    if (out == NULL || cap < 1 + 8) {
        return -1;
    }

    out[0] = REG_CMD;
    for (i = 7; i >= 0; i--) {
        out[1 + (7 - i)] = (uint8_t)((seq_ms >> (i * 8)) & 0xFFu);
    }
    pos = 9;
    pos = put_str(out, cap, pos, imei);
    pos = put_str(out, cap, pos, firmware);
    pos = put_str(out, cap, pos, iccid);
    return pos;
}

int reg_parse(const uint8_t *buf, int len, struct reg_reply *reply)
{
    uint64_t seq;
    int i;
    int n;

    if (buf == NULL || reply == NULL || len < 10 || buf[0] != REG_REPLY) {
        return -1;
    }

    seq = 0;
    for (i = 0; i < 8; i++) {
        seq = (seq << 8) | buf[1 + i];
    }
    reply->seq = seq;
    reply->code = buf[9];
    reply->cid[0] = '\0';
    if (reply->code != 0) {
        return 0;
    }
    if (len < 11) {
        return -1;
    }
    n = buf[10];
    if (n <= 0 || n >= (int)sizeof(reply->cid) || len < 11 + n) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        reply->cid[i] = (char)buf[11 + i];
    }
    reply->cid[n] = '\0';
    return 0;
}
