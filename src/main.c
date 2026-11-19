#include "config.h"
#include "log.h"
#include "n32_frame.h"
#include "reg_msg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define sleep(seconds) Sleep((seconds) * 1000u)
#else
#include <unistd.h>
#endif

/* 未注册成功之前，播报、亮屏、雷达联动和 485 转发都不做。 */
enum app_state {
    APP_WAIT_LINK = 0,
    APP_REGISTER,
    APP_RUN
};

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

    n = reg_build(out, sizeof(out), 0x000001976753A340ull,
                  "862413078981562", "HYK-DG01-250505D",
                  "89861124200043780101");
    return expect_bytes("reg", out, n, want, (int)sizeof(want));
}

static int selftest(void)
{
    if (selftest_n32() != 0 || selftest_reg() != 0) {
        return 1;
    }
    printf("selftest ok\n");
    return 0;
}

static void run_service(const char *path)
{
    struct app_config cfg;
    enum app_state state = APP_WAIT_LINK;

    if (config_load(&cfg, path) != 0) {
        log_error("打不开配置 %s，使用默认节点", path);
    }

    log_info("hydg02 启动。状态=等待上网。注册成功前不执行现场动作");
    log_info("4G %s / %s，屏 %s %u，485 %s %s，定位 %s，功放 GPIO%d",
             cfg.at_dev, cfg.net_if, cfg.n32_dev, cfg.n32_baud,
             cfg.rs485_dev, cfg.rs485_6_dev, cfg.gps_dev, cfg.amp_gpio);
    if (cfg.imei[0] == '\0' || cfg.mqtt_host[0] == '\0') {
        log_error("imei 或 mqtt_host 还是空的，填好 /etc/hydg02/hydg02.conf 再注册");
    }

    (void)state;
    while (1) {
        sleep(3600);
    }
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
