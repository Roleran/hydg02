#ifndef MODEM_H
#define MODEM_H

#include "config.h"

struct modem_id {
    char imei[32];
    char iccid[32];
};

/* 确认 SIM 和注网，拨号，直到 net_if 有 IPv4。身份从模组读出。 */
int modem_online(const struct app_config *cfg, struct modem_id *id);

#endif
