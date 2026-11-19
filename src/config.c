#include "config.h"

#include "board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *dst, int cap, const char *src)
{
    int i;

    for (i = 0; i < cap - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void set_defaults(struct app_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    copy_text(cfg->firmware, CFG_TEXT, "HY-DG02");
    cfg->mqtt_port = 1883;
    copy_text(cfg->n32_dev, CFG_TEXT, DEV_N32);
    cfg->n32_baud = N32_BAUD;
    copy_text(cfg->rs485_dev, CFG_TEXT, DEV_RS485);
    copy_text(cfg->rs485_6_dev, CFG_TEXT, DEV_RS485_6);
    cfg->rs485_baud = RS485_BAUD;
    copy_text(cfg->gps_dev, CFG_TEXT, DEV_GPS);
    cfg->gps_baud = GPS_BAUD;
    copy_text(cfg->at_dev, CFG_TEXT, DEV_AT);
    copy_text(cfg->net_if, CFG_TEXT, NET_IF);
    cfg->amp_gpio = AMP_GPIO;
    copy_text(cfg->voice_dir, (int)sizeof(cfg->voice_dir), "/etc/hydg02/voice");
}

static void apply_line(struct app_config *cfg, const char *key, const char *val)
{
    if (strcmp(key, "imei") == 0) {
        copy_text(cfg->imei, CFG_TEXT, val);
    } else if (strcmp(key, "iccid") == 0) {
        copy_text(cfg->iccid, CFG_TEXT, val);
    } else if (strcmp(key, "firmware") == 0) {
        copy_text(cfg->firmware, CFG_TEXT, val);
    } else if (strcmp(key, "mqtt_host") == 0) {
        copy_text(cfg->mqtt_host, CFG_TEXT, val);
    } else if (strcmp(key, "mqtt_port") == 0) {
        cfg->mqtt_port = atoi(val);
    } else if (strcmp(key, "http_url") == 0) {
        copy_text(cfg->http_url, (int)sizeof(cfg->http_url), val);
    } else if (strcmp(key, "apn") == 0) {
        copy_text(cfg->apn, CFG_TEXT, val);
    } else if (strcmp(key, "n32_dev") == 0) {
        copy_text(cfg->n32_dev, CFG_TEXT, val);
    } else if (strcmp(key, "n32_baud") == 0) {
        cfg->n32_baud = (unsigned)strtoul(val, NULL, 10);
    } else if (strcmp(key, "rs485_dev") == 0) {
        copy_text(cfg->rs485_dev, CFG_TEXT, val);
    } else if (strcmp(key, "rs485_6_dev") == 0) {
        copy_text(cfg->rs485_6_dev, CFG_TEXT, val);
    } else if (strcmp(key, "rs485_baud") == 0) {
        cfg->rs485_baud = (unsigned)strtoul(val, NULL, 10);
    } else if (strcmp(key, "gps_dev") == 0) {
        copy_text(cfg->gps_dev, CFG_TEXT, val);
    } else if (strcmp(key, "gps_baud") == 0) {
        cfg->gps_baud = (unsigned)strtoul(val, NULL, 10);
    } else if (strcmp(key, "at_dev") == 0) {
        copy_text(cfg->at_dev, CFG_TEXT, val);
    } else if (strcmp(key, "net_if") == 0) {
        copy_text(cfg->net_if, CFG_TEXT, val);
    } else if (strcmp(key, "amp_gpio") == 0) {
        cfg->amp_gpio = atoi(val);
    } else if (strcmp(key, "voice_dir") == 0) {
        copy_text(cfg->voice_dir, (int)sizeof(cfg->voice_dir), val);
    }
}

int config_load(struct app_config *cfg, const char *path)
{
    FILE *fp;
    char line[320];

    set_defaults(cfg);
    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key;
        char *val;
        char *end;

        key = line;
        while (*key == ' ' || *key == '\t') {
            key++;
        }
        if (*key == '#' || *key == '\n' || *key == '\0') {
            continue;
        }
        end = key + strlen(key);
        while (end > key && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *--end = '\0';
        }
        val = strchr(key, '=');
        if (val == NULL) {
            continue;
        }
        *val++ = '\0';
        apply_line(cfg, key, val);
    }

    fclose(fp);
    return 0;
}
