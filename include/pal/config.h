/*
 * include/pal/config.h - hydg02.conf 键值配置解析
 */
#ifndef PAL_CONFIG_H
#define PAL_CONFIG_H

#define CONF_MAX_KEY	32
#define CONF_MAX_VAL	192

/* 配置文件路径 (main 在 initcalls_run 之前赋值; 缺省 /etc/hydg02/hydg02.conf) */
extern const char *g_conf_path;

/* 载入配置文件到进程内静态表。返回 0 成功。可重复调用(重载)。 */
int conf_load(const char *path);

/* 取字符串值。未配置返回 def。 */
const char *conf_get(const char *key, const char *def);

/* 取整数值(十进制)。未配置或非法返回 def。 */
long conf_get_int(const char *key, long def);

/* 取布尔值: 1/yes/on 为真, 其余为假。未配置返回 def。 */
int conf_get_bool(const char *key, int def);

#endif /* PAL_CONFIG_H */
