#ifndef ONLINE_H
#define ONLINE_H

#include "config.h"

/* 上电上云。失败会隔 30 秒从头再来，正常情况不返回。 */
int online_run(const struct app_config *cfg);

#endif
