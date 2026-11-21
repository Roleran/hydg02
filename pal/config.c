/*
 * pal/config.c - 键值配置解析 (key=value, #注释, 空行忽略)
 */
#include <pal/config.h>
#include <sal/initcall.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* 配置文件路径 (main 可在 initcalls_run 前改写, 如传 argv[1]) */
const char *g_conf_path = "/etc/hydg02/hydg02.conf";

#define CONF_MAX_ENTRIES	64

struct conf_entry {
	char key[CONF_MAX_KEY];
	char val[CONF_MAX_VAL];
};

static struct conf_entry entries[CONF_MAX_ENTRIES];
static int n_entries;

static void trim(char *s)
{
	size_t len = strlen(s);

	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
	while (*s && isspace((unsigned char)*s))
		memmove(s, s + 1, strlen(s));
}

int conf_load(const char *path)
{
	FILE *fp;
	char line[256];

	n_entries = 0;

	fp = fopen(path, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		char *eq, *key, *val;

		trim(line);
		if (line[0] == '\0' || line[0] == '#')
			continue;

		eq = strchr(line, '=');
		if (!eq)
			continue;

		*eq = '\0';
		key = line;
		val = eq + 1;
		trim(key);
		trim(val);

		if (key[0] == '\0' || strlen(key) >= CONF_MAX_KEY ||
		    strlen(val) >= CONF_MAX_VAL)
			continue;

		if (n_entries >= CONF_MAX_ENTRIES)
			break;

		strncpy(entries[n_entries].key, key, CONF_MAX_KEY - 1);
		strncpy(entries[n_entries].val, val, CONF_MAX_VAL - 1);
		n_entries++;
	}

	fclose(fp);
	return 0;
}

const char *conf_get(const char *key, const char *def)
{
	int i;

	for (i = 0; i < n_entries; i++)
		if (strcmp(entries[i].key, key) == 0)
			return entries[i].val;
	return def;
}

long conf_get_int(const char *key, long def)
{
	const char *v = conf_get(key, NULL);
	char *end;
	long r;

	if (!v || v[0] == '\0')
		return def;

	r = strtol(v, &end, 0);		/* 支持 0x 前缀 */
	if (end == v)
		return def;
	return r;
}

int conf_get_bool(const char *key, int def)
{
	const char *v = conf_get(key, NULL);

	if (!v)
		return def;
	if (!strcmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"))
		return 1;
	return 0;
}

/* === initcall 注册 (EARLY: 其他模块的 conf_get 依赖本次载入) === */

static int conf_sysinit(void)
{
	if (conf_load(g_conf_path) != 0) {
		/* 开发板上配置缺失不算致命: 全部键走默认值 */
		fprintf(stderr, "conf: %s not found, using defaults\n",
			g_conf_path);
	}
	return 0;
}
initcall_early(conf_sysinit);
