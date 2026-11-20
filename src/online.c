#include "online.h"

#include "admin.h"
#include "bus.h"
#include "gps_fix.h"
#include "http_broker.h"
#include "led.h"
#include "loc_msg.h"
#include "log.h"
#include "modem.h"
#include "mqtt_cli.h"
#include "reg_msg.h"
#include "talk.h"
#include "trig.h"
#include "voice.h"

#include <stdint.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define sleep(seconds) Sleep((seconds) * 1000u)
#else
#include <sys/time.h>
#include <unistd.h>
#endif

#define RETRY_SEC 30

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return 0;
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
#endif
}

static int wait_reply(struct mqtt *mq, uint64_t seq, struct reg_reply *reply)
{
    char topic[96];
    unsigned char payload[128];
    int payload_len;
    int left;

    left = 15000;
    while (left > 0) {
        int step = left > 1000 ? 1000 : left;
        int got = mqtt_wait_publish(mq, topic, (int)sizeof(topic),
                                    payload, &payload_len, (int)sizeof(payload), step);

        left -= step;
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            continue;
        }
        if (reg_parse(payload, payload_len, reply) != 0) {
            log_info("收到 %s，%d 字节，不是注册回复", topic, payload_len);
            continue;
        }
        if (reply->seq != seq) {
            log_error("注册回复的序列号对不上");
            continue;
        }
        return 0;
    }
    return -1;
}

struct link {
    struct mqtt *mq;
    struct voice *voice;
    struct talk *talk;
    struct led *led;
    struct trig *trig;
    struct bus *bus;
    struct admin *admin;
    char upgrade_up[96];
    char up[96];
    char msg[96];
    char upgrade[96];
};

static int link_pump(void *ctx, int idle_ms);

static int link_publish(void *ctx, const uint8_t *data, int n)
{
    struct link *lk = ctx;

    return mqtt_publish(lk->mq, lk->up, data, n);
}

static int link_publish_upgrade(void *ctx, const uint8_t *data, int n)
{
    struct link *lk = ctx;

    return mqtt_publish(lk->mq, lk->upgrade_up, data, n);
}

static int link_dispatch(struct link *lk, const char *topic,
                         const unsigned char *payload, int n)
{
    int handled;

    if (strcmp(topic, lk->msg) == 0) {
        if (admin_is_busy() && (n < 1 || payload[0] != ADMIN_VERSION)) {
            if (n >= 9) {
                uint8_t ack[16];
                uint64_t seq;
                int i;
                int bytes;

                seq = 0;
                for (i = 0; i < 8; i++) {
                    seq = (seq << 8) | payload[1 + i];
                }
                bytes = voice_ack(ack, (int)sizeof(ack), seq, payload[0], VOICE_FAIL);
                if (bytes > 0) {
                    link_publish(lk, ack, bytes);
                }
            }
            log_info("升级进行中，命令 %02X 不执行", n > 0 ? payload[0] : 0);
            return 0;
        }
        handled = admin_on_packet(lk->admin, payload, n, link_publish, lk);
        if (handled != 1) {
            return handled;
        }
        handled = talk_on_packet(lk->talk, payload, n, link_publish, link_pump, lk);
        if (handled != 1) {
            return handled;
        }
        handled = led_on_packet(lk->led, payload, n, link_publish, lk);
        if (handled != 1) {
            return handled;
        }
        handled = trig_on_packet(lk->trig, payload, n, link_publish, link_pump, lk);
        if (handled != 1) {
            return handled;
        }
        handled = bus_on_packet(lk->bus, payload, n, link_publish, link_pump, lk);
        if (handled != 1) {
            return handled;
        }
        return voice_on_packet(lk->voice, payload, n, link_publish, link_pump, lk);
    }
    if (strcmp(topic, lk->upgrade) == 0) {
        return admin_on_upgrade(lk->admin, payload, n, link_publish_upgrade, link_pump, lk);
    }
    log_info("收到 %s，%d 字节，不处理", topic, n);
    return 0;
}

#ifndef _WIN32
/* 下载和对讲会长时间只调用 idle_ms=0，这里补心跳，避免 60 秒被断开。 */
static int keepalive(struct link *lk)
{
    struct timeval now;
    static struct timeval last;
    long long gap;

    gettimeofday(&now, NULL);
    if (last.tv_sec == 0) {
        last = now;
        return 0;
    }
    gap = (long long)(now.tv_sec - last.tv_sec) * 1000LL +
          (long long)(now.tv_usec - last.tv_usec) / 1000LL;
    if (gap < 20000) {
        return 0;
    }
    last = now;
    return mqtt_ping(lk->mq);
}
#endif

/* idle_ms 为 0 时只取走已经到达的指令。等待按 200 毫秒切开，以便应答屏的触发。 */
static int link_pump(void *ctx, int idle_ms)
{
    struct link *lk = ctx;
    char topic[96];
    unsigned char payload[1024];
    int n;
    int got;
    int left;

    if (idle_ms < 0) {
        return mqtt_ping(lk->mq);
    }
#ifndef _WIN32
    if (keepalive(lk) != 0) {
        return -1;
    }
#endif
    trig_service(lk->trig, link_publish, link_pump, lk);
    if (idle_ms == 0) {
        for (;;) {
            got = mqtt_wait_publish(lk->mq, topic, (int)sizeof(topic),
                                    payload, &n, (int)sizeof(payload), 0);
            if (got <= 0) {
                return got;
            }
            if (link_dispatch(lk, topic, payload, n) != 0) {
                return -1;
            }
        }
    }
    left = idle_ms;
    while (left > 0) {
        int step = left > 200 ? 200 : left;

        got = mqtt_wait_publish(lk->mq, topic, (int)sizeof(topic),
                                payload, &n, (int)sizeof(payload), step);
        if (got < 0) {
            return -1;
        }
        if (got > 0) {
            return link_dispatch(lk, topic, payload, n);
        }
        left -= step;
        trig_service(lk->trig, link_publish, link_pump, lk);
    }
    if (idle_ms >= 1000) {
        return mqtt_ping(lk->mq);
    }
    return 0;
}

static void hold_link(struct link *lk)
{
    while (link_pump(lk, 20000) == 0) {
    }
    log_error("MQTT 连接断开");
}

int online_run(const struct app_config *cfg)
{
#ifdef _WIN32
    (void)cfg;
    log_error("上电上云要在主板上运行");
    return -1;
#else
    struct voice voice;
    struct talk talk;
    struct led panel;
    struct trig trigger;
    struct bus rs485;
    struct admin board;

    if (cfg == NULL) {
        return -1;
    }
    voice_init(&voice, cfg);
    talk_init(&talk, &voice);
    led_init(&panel, cfg);
    trig_init(&trigger, &voice, &panel, cfg->n32_dev, cfg->n32_baud);
    bus_init(&rs485, cfg);
    admin_init(&board, cfg, &voice, &rs485);
    while (1) {
        struct modem_id id;
        struct mqtt *mq;
        struct reg_reply reply;
        struct gps_fix fix;
        unsigned char packet[160];
        char broker_host[64];
        char down[96];
        char up[96];
        char msg_up[96];
        char msg[96];
        char upgrade[96];
        int broker_port;
        int n;
        uint64_t seq;

        if (cfg->firmware[0] == '\0') {
            log_error("firmware 还是空的");
            sleep(RETRY_SEC);
            continue;
        }
        if (cfg->http_url[0] == '\0' && cfg->mqtt_host[0] == '\0') {
            log_error("http_url 和 mqtt_host 都是空的");
            sleep(RETRY_SEC);
            continue;
        }
        log_info("开始上电上云");
        if (modem_online(cfg, &id) != 0) {
            sleep(RETRY_SEC);
            continue;
        }
        if (broker_resolve(cfg, broker_host, (int)sizeof(broker_host), &broker_port) != 0) {
            log_error("没有拿到 MQTT 地址");
            sleep(RETRY_SEC);
            continue;
        }
        log_info("MQTT 地址 %s:%d", broker_host, broker_port);

        snprintf(down, sizeof(down), "raw/v1/cmd/auth/%s", id.imei);
        snprintf(up, sizeof(up), "raw/v1/resp/auth/%s", id.imei);
        mq = mqtt_connect(broker_host, broker_port, id.imei);
        if (mq == NULL) {
            log_error("连接 MQTT %s:%d 失败", broker_host, broker_port);
            sleep(RETRY_SEC);
            continue;
        }
        if (mqtt_subscribe(mq, down) != 0) {
            log_error("订阅 %s 失败", down);
            mqtt_close(mq);
            sleep(RETRY_SEC);
            continue;
        }

        seq = now_ms();
        n = reg_build(packet, (int)sizeof(packet), seq, id.imei, cfg->firmware, id.iccid);
        if (n < 0 || mqtt_publish(mq, up, packet, n) != 0) {
            log_error("发送注册报文失败");
            mqtt_close(mq);
            sleep(RETRY_SEC);
            continue;
        }
        log_info("已发送注册，IMEI=%s", id.imei);
        if (wait_reply(mq, seq, &reply) != 0) {
            log_error("没有等到注册回复");
            mqtt_close(mq);
            sleep(RETRY_SEC);
            continue;
        }
        if (reply.code != 0) {
            log_error("注册被拒绝，30 秒后重试");
            mqtt_close(mq);
            sleep(RETRY_SEC);
            continue;
        }

        log_info("注册成功，CID=%s", reply.cid);
        snprintf(msg, sizeof(msg), "raw/v1/cmd/msg/%s", reply.cid);
        snprintf(upgrade, sizeof(upgrade), "raw/v1/cmd/upgrade/%s", reply.cid);
        if (mqtt_subscribe2(mq, msg, upgrade) != 0) {
            log_error("订阅业务主题失败");
            mqtt_close(mq);
            sleep(RETRY_SEC);
            continue;
        }

        snprintf(msg_up, sizeof(msg_up), "raw/v1/resp/msg/%s", reply.cid);
        if (gps_read_once(cfg->gps_dev, cfg->gps_baud, &fix, 20000) != 0) {
            log_error("打不开定位口 %s，按无定位上报", cfg->gps_dev);
            memset(&fix, 0, sizeof(fix));
        }
        seq = now_ms();
        n = loc_build(packet, (int)sizeof(packet), seq, &fix);
        if (n < 0 || mqtt_publish(mq, msg_up, packet, n) != 0) {
            log_error("发送定位失败");
            mqtt_close(mq);
            sleep(RETRY_SEC);
            continue;
        }
        if (packet[9] != 0) {
            log_info("已上报定位 时间=%s 纬度=%s%c 经度=%s%c",
                     fix.utc, fix.lat, fix.ns, fix.lon, fix.ew);
        } else {
            log_info("已上报无定位");
        }

        {
            struct link lk;

            memset(&lk, 0, sizeof(lk));
            lk.mq = mq;
            lk.voice = &voice;
            lk.talk = &talk;
            lk.led = &panel;
            lk.trig = &trigger;
            lk.bus = &rs485;
            lk.admin = &board;
            led_sync(&panel);
            snprintf(lk.up, sizeof(lk.up), "%s", msg_up);
            snprintf(lk.msg, sizeof(lk.msg), "%s", msg);
            snprintf(lk.upgrade, sizeof(lk.upgrade), "%s", upgrade);
            snprintf(lk.upgrade_up, sizeof(lk.upgrade_up), "raw/v1/resp/upgrade/%s", reply.cid);
            hold_link(&lk);
        }
        mqtt_close(mq);
        log_info("30 秒后重新上线");
        sleep(RETRY_SEC);
    }
#endif
}
