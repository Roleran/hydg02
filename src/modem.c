#include "modem.h"

#include "at_port.h"
#include "log.h"

#ifndef _WIN32

#include <arpa/inet.h>
#include <ctype.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int ifname_ok(const char *name)
{
    int i;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c)) {
            return 0;
        }
        if (i >= 15) {
            return 0;
        }
    }
    return 1;
}

static int net_has_addr(const char *ifname)
{
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 0;
    }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        close(fd);
        return 0;
    }
    close(fd);
    return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr != 0;
}

static void copy_digits(char *dst, int cap, const char *src, int min_n, int max_n)
{
    int n;
    int i;
    const char *p;

    dst[0] = '\0';
    p = src;
    while (*p != '\0') {
        while (*p != '\0' && !isdigit((unsigned char)*p)) {
            p++;
        }
        n = 0;
        while (isdigit((unsigned char)p[n])) {
            n++;
        }
        if (n >= min_n && n <= max_n && n < cap) {
            for (i = 0; i < n; i++) {
                dst[i] = p[i];
            }
            dst[n] = '\0';
            return;
        }
        p += n;
    }
}

/* 注网状态在第一个逗号后面。1 和 5 都算已注册。 */
static int net_registered(const char *resp)
{
    const char *comma;
    int stat;

    comma = strchr(resp, ',');
    if (comma == NULL || !isdigit((unsigned char)comma[1])) {
        return 0;
    }
    stat = comma[1] - '0';
    return stat == 1 || stat == 5;
}

static int wait_registered(int fd)
{
    char resp[256];
    int i;

    for (i = 0; i < 15; i++) {
        if (at_exchange(fd, "AT+CEREG?", resp, sizeof(resp), 2000) == 0 &&
            net_registered(resp)) {
            return 0;
        }
        if (at_exchange(fd, "AT+CREG?", resp, sizeof(resp), 2000) == 0 &&
            net_registered(resp)) {
            return 0;
        }
        sleep(2);
    }
    return -1;
}

static int dhcp_once(const char *ifname)
{
    char cmd[64];

    snprintf(cmd, sizeof(cmd), "udhcpc -n -q -t 5 -i %s", ifname);
    return system(cmd) == 0 ? 0 : -1;
}

int modem_online(const struct app_config *cfg, struct modem_id *id)
{
    int fd;
    char resp[512];
    char cgd[96];
    int i;

    if (cfg == NULL || id == NULL || !ifname_ok(cfg->net_if)) {
        return -1;
    }
    memset(id, 0, sizeof(*id));
    fd = at_open(cfg->at_dev, 115200);
    if (fd < 0) {
        log_error("打不开 AT 口 %s", cfg->at_dev);
        return -1;
    }
    if (at_exchange(fd, "AT", resp, sizeof(resp), 2000) != 0) {
        log_error("AT 口没有应答");
        at_close(fd);
        return -1;
    }
    at_exchange(fd, "ATE0", resp, sizeof(resp), 1000);
    if (at_exchange(fd, "AT+CPIN?", resp, sizeof(resp), 3000) != 0 ||
        strstr(resp, "READY") == NULL) {
        log_error("SIM 未就绪");
        at_close(fd);
        return -1;
    }
    if (at_exchange(fd, "AT+CSQ", resp, sizeof(resp), 2000) == 0) {
        log_info("信号 %s", resp);
    }
    if (wait_registered(fd) != 0) {
        log_error("尚未注册到运营商");
        at_close(fd);
        return -1;
    }
    if (at_exchange(fd, "AT+CGSN", resp, sizeof(resp), 2000) != 0) {
        log_error("读不到 IMEI");
        at_close(fd);
        return -1;
    }
    copy_digits(id->imei, (int)sizeof(id->imei), resp, 15, 15);
    if (at_exchange(fd, "AT+QCCID", resp, sizeof(resp), 2000) != 0) {
        log_error("读不到 ICCID");
        at_close(fd);
        return -1;
    }
    copy_digits(id->iccid, (int)sizeof(id->iccid), resp, 19, 20);
    if (id->imei[0] == '\0' || id->iccid[0] == '\0') {
        log_error("IMEI 或 ICCID 不是数字");
        at_close(fd);
        return -1;
    }
    log_info("模组 IMEI=%s ICCID=%s", id->imei, id->iccid);

    if (cfg->apn[0] != '\0') {
        snprintf(cgd, sizeof(cgd), "AT+CGDCONT=1,\"IP\",\"%s\"", cfg->apn);
        if (at_exchange(fd, cgd, resp, sizeof(resp), 3000) != 0) {
            log_error("设置 APN 失败");
            at_close(fd);
            return -1;
        }
    }
    if (at_exchange(fd, "AT+QNETDEVCTL=1,1,1", resp, sizeof(resp), 8000) != 0) {
        log_info("QNETDEVCTL 未成功，改试 CGACT");
        at_exchange(fd, "AT+CGACT=1,1", resp, sizeof(resp), 8000);
    }
    at_close(fd);

    for (i = 0; i < 10; i++) {
        if (net_has_addr(cfg->net_if)) {
            log_info("%s 已有地址", cfg->net_if);
            return 0;
        }
        sleep(1);
    }
    log_info("向 %s 请求地址", cfg->net_if);
    dhcp_once(cfg->net_if);
    for (i = 0; i < 10; i++) {
        if (net_has_addr(cfg->net_if)) {
            log_info("%s 已有地址", cfg->net_if);
            return 0;
        }
        sleep(1);
    }
    log_error("%s 没有 IPv4 地址", cfg->net_if);
    return -1;
}

#else

int modem_online(const struct app_config *cfg, struct modem_id *id)
{
    (void)cfg;
    (void)id;
    return -1;
}

#endif
