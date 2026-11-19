#ifndef CONFIG_H
#define CONFIG_H

#define CFG_TEXT 64

struct app_config {
    char imei[CFG_TEXT];
    char iccid[CFG_TEXT];
    char firmware[CFG_TEXT];
    char mqtt_host[CFG_TEXT];
    int mqtt_port;
    char n32_dev[CFG_TEXT];
    unsigned n32_baud;
    char rs485_dev[CFG_TEXT];
    char rs485_6_dev[CFG_TEXT];
    unsigned rs485_baud;
    char gps_dev[CFG_TEXT];
    unsigned gps_baud;
    char at_dev[CFG_TEXT];
    char net_if[CFG_TEXT];
    int amp_gpio;
};

/* 先填板上的默认节点，再用配置文件覆盖。文件打不开返回 -1。 */
int config_load(struct app_config *cfg, const char *path);

#endif
