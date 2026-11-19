#include "admin.h"
#include "bus.h"
#include "config.h"
#include "gps_fix.h"
#include "led.h"
#include "http_broker.h"
#include "loc_msg.h"
#include "log.h"
#include "md5.h"
#include "n32_frame.h"
#include "online.h"
#include "reg_msg.h"
#include "talk.h"
#include "trig.h"
#include "voice.h"

#include <stdio.h>
#include <string.h>

static int expect_bytes(const char *name, const uint8_t *got, int got_len,
                        const uint8_t *want, int want_len)
{
    int i;

    if (got_len != want_len) {
        fprintf(stderr, "%s length %d, want %d\n", name, got_len, want_len);
        return -1;
    }
    for (i = 0; i < want_len; i++) {
        if (got[i] != want[i]) {
            fprintf(stderr, "%s byte %d is %02X, want %02X\n",
                    name, i, got[i], want[i]);
            return -1;
        }
    }
    return 0;
}

/* 对照协议里已经写死的两帧，确认组帧和 CRC。 */
static int selftest_n32(void)
{
    uint8_t out[N32_FRAME_MAX];
    struct n32_frame frame;
    int used;
    int n;
    int rc;
    const uint8_t ping[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x01, 0x00, 0x00, 0x49, 0xFC
    };
    const uint8_t text[] = {
        0xA5, 0x5A, 0x01, 0x11, 0x02, 0x05, 0x00,
        0x02, 0x50, 0xE5, 0x81, 0x9C, 0x2C, 0x45
    };
    const uint8_t text_data[] = { 0x02, 0x50, 0xE5, 0x81, 0x9C };

    n = n32_frame_build(out, sizeof(out), N32_CMD_PING, 1, NULL, 0);
    if (expect_bytes("ping", out, n, ping, (int)sizeof(ping)) != 0) {
        return -1;
    }
    rc = n32_frame_parse(out, n, &frame, &used);
    if (rc != 1 || used != n || frame.cmd != N32_CMD_PING || frame.len != 0) {
        fprintf(stderr, "ping parse failed\n");
        return -1;
    }

    n = n32_frame_build(out, sizeof(out), N32_CMD_TEXT, 2, text_data, 5);
    if (expect_bytes("text", out, n, text, (int)sizeof(text)) != 0) {
        return -1;
    }
    return 0;
}

/* 对照 MQTT 协议 2.0.0 里的 0x81 示例。 */
static int selftest_reg(void)
{
    uint8_t out[128];
    int n;
    const uint8_t want[] = {
        0x81,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x0F, 0x38, 0x36, 0x32, 0x34, 0x31, 0x33, 0x30,
        0x37, 0x38, 0x39, 0x38, 0x31, 0x35, 0x36, 0x32,
        0x10, 0x48, 0x59, 0x4B, 0x2D, 0x44, 0x47, 0x30,
        0x31, 0x2D, 0x32, 0x35, 0x30, 0x35, 0x30, 0x35, 0x44,
        0x14, 0x38, 0x39, 0x38, 0x36, 0x31, 0x31, 0x32,
        0x34, 0x32, 0x30, 0x30, 0x30, 0x34, 0x33, 0x37,
        0x38, 0x30, 0x31, 0x30, 0x31
    };

    struct reg_reply reply;
    const uint8_t reply_bytes[] = {
        0x01,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x00,
        0x0B, 0x31, 0x30, 0x32, 0x30, 0x37, 0x35, 0x32, 0x39, 0x38, 0x30, 0x31
    };

    n = reg_build(out, sizeof(out), 0x000001976753A340ull,
                  "862413078981562", "HYK-DG01-250505D",
                  "89861124200043780101");
    if (expect_bytes("reg", out, n, want, (int)sizeof(want)) != 0) {
        return -1;
    }
    if (reg_parse(reply_bytes, (int)sizeof(reply_bytes), &reply) != 0) {
        fprintf(stderr, "reg reply parse failed\n");
        return -1;
    }
    if (reply.seq != 0x000001976753A340ull || reply.code != 0 ||
        strcmp(reply.cid, "10207529801") != 0) {
        fprintf(stderr, "reg reply fields mismatch\n");
        return -1;
    }
    return 0;
}

/* 3112.3456 N、12130.0000 E、083015.00 换成 1e-7 度和时分秒。 */
static int selftest_loc(void)
{
    struct gps_fix fix;
    uint8_t out[32];
    int n;
    const uint8_t want[] = {
        0x82,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x01,
        0x12, 0x99, 0x9F, 0x00,
        0x48, 0x6B, 0x6D, 0xC0,
        0x00, 0x01, 0x44, 0x47
    };

    memset(&fix, 0, sizeof(fix));
    fix.valid = 1;
    memcpy(fix.utc, "083015.00", 10);
    memcpy(fix.lat, "3112.3456", 10);
    memcpy(fix.lon, "12130.0000", 11);
    fix.ns = 'N';
    fix.ew = 'E';
    n = loc_build(out, (int)sizeof(out), 0x000001976753A340ull, &fix);
    return expect_bytes("loc", out, n, want, (int)sizeof(want));
}

static int selftest_broker(void)
{
    char host[64];
    char path[64];
    int port;

    if (broker_split_url("http://dir.example.com:8080/mqtt/addr",
                         host, (int)sizeof(host), &port, path, (int)sizeof(path)) != 0 ||
        port != 8080 || strcmp(host, "dir.example.com") != 0 ||
        strcmp(path, "/mqtt/addr") != 0) {
        fprintf(stderr, "broker url split failed\n");
        return -1;
    }
    if (broker_parse_endpoint("  mqtt.example.com:1883\r\n",
                              host, (int)sizeof(host), &port) != 0 ||
        port != 1883 || strcmp(host, "mqtt.example.com") != 0) {
        fprintf(stderr, "broker endpoint parse failed\n");
        return -1;
    }
    return 0;
}

static int selftest_voice(void)
{
    struct voice_cmd cmd;
    uint8_t out[32];
    uint8_t md[16];
    uint8_t packet[64];
    int n;
    const uint8_t abc_md5[] = {
        0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
        0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72
    };
    const uint8_t ack[] = {
        0xA0,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x20, 0x00
    };
    const char *url = "http://a.example.com/1.wav";

    if (md5_of("abc", 3, md) != 0 ||
        expect_bytes("md5", md, 16, abc_md5, 16) != 0) {
        return -1;
    }
    packet[0] = VOICE_SET;
    packet[1] = 0x00;
    packet[2] = 0x00;
    packet[3] = 0x01;
    packet[4] = 0x97;
    packet[5] = 0x67;
    packet[6] = 0x53;
    packet[7] = 0xA3;
    packet[8] = 0x40;
    packet[9] = 0x00;
    if (voice_parse(packet, 10, &cmd) != 0 || cmd.seq != 0x000001976753A340ull ||
        cmd.on != 0) {
        fprintf(stderr, "voice set parse failed\n");
        return -1;
    }
    n = voice_ack(out, (int)sizeof(out), cmd.seq, VOICE_SET, VOICE_OK);
    if (expect_bytes("voice ack", out, n, ack, (int)sizeof(ack)) != 0) {
        return -1;
    }
    if (!voice_known_suffix(url) || voice_known_suffix("http://a.example.com/1.txt")) {
        fprintf(stderr, "voice suffix failed\n");
        return -1;
    }
    packet[0] = VOICE_FETCH;
    packet[9] = 1;
    memset(packet + 10, 0x11, 16);
    packet[26] = 0;
    packet[27] = (uint8_t)strlen(url);
    memcpy(packet + 28, url, strlen(url));
    if (voice_parse(packet, 28 + (int)strlen(url), &cmd) != 0 || cmd.slot != 1 ||
        strcmp(cmd.url, url) != 0 || cmd.md5[0] != 0x11) {
        fprintf(stderr, "voice fetch parse failed\n");
        return -1;
    }
    packet[9] = 4;
    if (voice_parse(packet, 28 + (int)strlen(url), &cmd) == 0) {
        fprintf(stderr, "voice slot 4 should fail\n");
        return -1;
    }
    return 0;
}

static int selftest_talk(void)
{
    struct talk_cmd cmd;
    uint8_t packet[40];
    const char *host = "zlm.example.com";
    int n;

    memset(packet, 0, sizeof(packet));
    packet[0] = TALK_START;
    packet[1] = 0x00;
    packet[2] = 0x00;
    packet[3] = 0x01;
    packet[4] = 0x97;
    packet[5] = 0x67;
    packet[6] = 0x53;
    packet[7] = 0xA3;
    packet[8] = 0x40;
    packet[9] = 0x27;
    packet[10] = 0x10;
    packet[11] = (uint8_t)strlen(host);
    memcpy(packet + 12, host, strlen(host));
    n = 12 + (int)strlen(host);
    if (talk_parse(packet, n, &cmd) != 0 || cmd.port != 10000 ||
        cmd.seq != 0x000001976753A340ull || strcmp(cmd.host, host) != 0) {
        fprintf(stderr, "talk start parse failed\n");
        return -1;
    }
    packet[0] = TALK_STOP;
    if (talk_parse(packet, 9, &cmd) != 0 || cmd.cmd != TALK_STOP) {
        fprintf(stderr, "talk stop parse failed\n");
        return -1;
    }
    packet[0] = TALK_START;
    packet[9] = 0;
    packet[10] = 0;
    if (talk_parse(packet, n, &cmd) == 0) {
        fprintf(stderr, "talk port 0 should fail\n");
        return -1;
    }
    return 0;
}

static int selftest_led(void)
{
    struct led_cmd cmd;
    struct n32_frame frame;
    uint8_t packet[32];
    uint8_t out[32];
    uint8_t data[8];
    uint8_t built[32];
    int used;
    int n;
    const uint8_t stop_utf8[] = { 0xE5, 0x81, 0x9C };
    const uint8_t state[] = {
        0xA2,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x00, 0x50, 0x00
    };

    memset(packet, 0, sizeof(packet));
    packet[0] = LED_TEXT;
    packet[1] = 0x00;
    packet[2] = 0x00;
    packet[3] = 0x01;
    packet[4] = 0x97;
    packet[5] = 0x67;
    packet[6] = 0x53;
    packet[7] = 0xA3;
    packet[8] = 0x40;
    packet[9] = 3;
    memcpy(packet + 10, stop_utf8, 3);
    if (led_parse(packet, 13, &cmd) != 0 || cmd.text_len != 3 ||
        cmd.seq != 0x000001976753A340ull || memcmp(cmd.text, stop_utf8, 3) != 0) {
        fprintf(stderr, "led text parse failed\n");
        return -1;
    }
    packet[0] = LED_SPEED;
    packet[9] = 80;
    if (led_parse(packet, 10, &cmd) != 0 || cmd.speed != 80) {
        fprintf(stderr, "led speed parse failed\n");
        return -1;
    }
    packet[9] = 10;
    if (led_parse(packet, 10, &cmd) == 0) {
        fprintf(stderr, "led speed 10 should fail\n");
        return -1;
    }
    n = led_state(out, (int)sizeof(out), 0x000001976753A340ull, 0, 80, NULL, 0);
    if (expect_bytes("led state", out, n, state, (int)sizeof(state)) != 0) {
        return -1;
    }
    data[0] = N32_DIR_RTL;
    data[1] = 80;
    memcpy(data + 2, stop_utf8, 3);
    n = n32_frame_build(built, (int)sizeof(built), N32_CMD_TEXT, 2, data, 5);
    if (n32_frame_parse(built, n, &frame, &used) != 1 || used != n ||
        frame.data[0] != N32_DIR_RTL || frame.data[1] != 80) {
        fprintf(stderr, "led rtl frame failed\n");
        return -1;
    }
    return 0;
}

static int selftest_trig(void)
{
    struct trig_cmd cmd;
    struct trig trig;
    uint8_t packet[16];
    uint8_t out[16];
    int n;
    const uint8_t state[] = {
        0xA3,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x00, 0x01, 0x07, 0x00, 0x12, 0x00
    };

    if (!trig_allows(0, 7 * 60, 18 * 60, 12 * 60)) {
        fprintf(stderr, "timer off should allow\n");
        return -1;
    }
    if (!trig_allows(1, 7 * 60, 18 * 60, 8 * 60) ||
        trig_allows(1, 7 * 60, 18 * 60, 18 * 60)) {
        fprintf(stderr, "day window failed\n");
        return -1;
    }
    if (!trig_allows(1, 22 * 60, 6 * 60, 23 * 60) ||
        trig_allows(1, 22 * 60, 6 * 60, 12 * 60)) {
        fprintf(stderr, "overnight window failed\n");
        return -1;
    }
    memset(&trig, 0, sizeof(trig));
    trig.mode = TRIG_RADAR;
    trig.timer_on = 1;
    trig.start_h = 7;
    trig.end_h = 18;
    n = trig_state(out, (int)sizeof(out), 0x000001976753A340ull, &trig);
    if (expect_bytes("trig state", out, n, state, (int)sizeof(state)) != 0) {
        return -1;
    }
    memset(packet, 0, sizeof(packet));
    packet[0] = TRIG_WINDOW;
    packet[1] = 0x00;
    packet[2] = 0x00;
    packet[3] = 0x01;
    packet[4] = 0x97;
    packet[5] = 0x67;
    packet[6] = 0x53;
    packet[7] = 0xA3;
    packet[8] = 0x40;
    packet[9] = 1;
    packet[10] = 7;
    packet[12] = 18;
    if (trig_parse(packet, 14, &cmd) != 0 || cmd.start_h != 7 || cmd.end_h != 18) {
        fprintf(stderr, "trig window parse failed\n");
        return -1;
    }
    packet[10] = 24;
    if (trig_parse(packet, 14, &cmd) == 0) {
        fprintf(stderr, "trig hour 24 should fail\n");
        return -1;
    }
    return 0;
}

static int selftest_bus(void)
{
    struct bus_cmd cmd;
    uint8_t packet[20];
    uint8_t out[20];
    int n;
    const uint8_t reply[] = {
        0xA5,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x00, 0x00, 0x02, 0x01, 0x03
    };

    memset(packet, 0, sizeof(packet));
    packet[0] = BUS_XFER;
    packet[1] = 0x00;
    packet[2] = 0x00;
    packet[3] = 0x01;
    packet[4] = 0x97;
    packet[5] = 0x67;
    packet[6] = 0x53;
    packet[7] = 0xA3;
    packet[8] = 0x40;
    packet[9] = 2;
    packet[10] = 0x01;
    packet[11] = 0xF4;
    packet[12] = 0x00;
    packet[13] = 2;
    packet[14] = 0x01;
    packet[15] = 0x03;
    if (bus_parse(packet, 16, &cmd) != 0 || cmd.port != 2 || cmd.timeout_ms != 500 ||
        cmd.data_len != 2 || cmd.data[0] != 0x01 || cmd.data[1] != 0x03) {
        fprintf(stderr, "bus parse failed\n");
        return -1;
    }
    n = bus_reply(out, (int)sizeof(out), cmd.seq, 0, cmd.data, cmd.data_len);
    if (expect_bytes("bus reply", out, n, reply, (int)sizeof(reply)) != 0) {
        return -1;
    }
    packet[9] = 3;
    if (bus_parse(packet, 16, &cmd) == 0) {
        fprintf(stderr, "bus port 3 should fail\n");
        return -1;
    }
    return 0;
}

static int selftest_admin(void)
{
    struct admin_cmd cmd;
    uint8_t packet[64];
    uint8_t out[24];
    int n;
    const char *url = "http://a.example.com/a.bin";
    const uint8_t ver[] = {
        0xA6,
        0x00, 0x00, 0x01, 0x97, 0x67, 0x53, 0xA3, 0x40,
        0x07, 'H', 'Y', '-', 'D', 'G', '0', '2'
    };

    n = admin_version(out, (int)sizeof(out), 0x000001976753A340ull, "HY-DG02");
    if (expect_bytes("version", out, n, ver, (int)sizeof(ver)) != 0) {
        return -1;
    }
    memset(packet, 0, sizeof(packet));
    packet[0] = ADMIN_UPGRADE;
    packet[1] = 0x00;
    packet[2] = 0x00;
    packet[3] = 0x01;
    packet[4] = 0x97;
    packet[5] = 0x67;
    packet[6] = 0x53;
    packet[7] = 0xA3;
    packet[8] = 0x40;
    memset(packet + 9, 0x22, 16);
    packet[25] = 0;
    packet[26] = (uint8_t)strlen(url);
    if (27 + (int)strlen(url) > (int)sizeof(packet)) {
        fprintf(stderr, "admin packet buffer short\n");
        return -1;
    }
    memcpy(packet + 27, url, strlen(url));
    if (admin_parse(packet, 27 + (int)strlen(url), &cmd) != 0 ||
        strcmp(cmd.url, url) != 0 || cmd.md5[0] != 0x22) {
        fprintf(stderr, "admin upgrade parse failed\n");
        return -1;
    }
    packet[0] = ADMIN_REBOOT;
    if (admin_parse(packet, 9, &cmd) != 0 || cmd.cmd != ADMIN_REBOOT) {
        fprintf(stderr, "admin reboot parse failed\n");
        return -1;
    }
    return 0;
}

static int selftest(void)
{
    if (selftest_n32() != 0 || selftest_reg() != 0 ||
        selftest_loc() != 0 || selftest_broker() != 0 ||
        selftest_voice() != 0 || selftest_talk() != 0 ||
        selftest_led() != 0 || selftest_trig() != 0 ||
        selftest_bus() != 0 || selftest_admin() != 0) {
        return 1;
    }
    printf("selftest ok\n");
    return 0;
}

static void run_service(const char *path)
{
    struct app_config cfg;

    if (config_load(&cfg, path) != 0) {
        log_error("打不开配置 %s，使用默认节点", path);
    }
    log_info("hydg02 启动。注册成功前不执行现场动作");
    online_run(&cfg);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        return selftest();
    }
    if (argc < 2) {
        log_error("用法: hydg02 <配置文件>  或  hydg02 --selftest");
        return 1;
    }
    run_service(argv[1]);
    return 0;
}
