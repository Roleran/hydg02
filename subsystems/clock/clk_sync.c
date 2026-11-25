/*
 * subsystems/clock/clk_sync.c — 系统时钟同步
 *
 * 优先级: GPS/北斗 (NMEA RMC) → 4G 模组 (AT+CCLK?)
 *
 * 场景: MT7628 无 RTC 电池, 每次重启时钟回到内核构建时间,
 *       导致 TLS 证书校验失败 (-3)。
 *       本模块在 initcall 阶段完成首次同步 (保证后续 HTTPS 可用),
 *       pumpcall 阶段周期重试 (时钟丢失时兜底)。
 *
 * 架构位置: subsystems/clock (业务层, 协调 GPS + EC200A 两个硬件源)
 * GPS: 直接读 /dev/gps (CH9437 ch7, 无人竞争)
 * 4G:  直接读 /dev/ttyUSB2 (init 阶段无竞争; pump 阶段有极小概率与
 *       ec200a 驱动抢 AT 响应, 驱动有重试机制可自愈)
 */

#include <sal/initcall.h>
#include <pal/config.h>
#include <pal/sysio.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <sys/time.h>

/* === 配置 === */

#define CLK_GPS_WAIT_MS		3000	/* GPS NMEA 等待 */
#define CLK_AT_TIMEOUT_MS	2000	/* 4G AT 响应超时 */
#define CLK_RETRY_MS		60000	/* 时钟无效时重试间隔 */
#define CLK_RESYNC_DELAY_MS	30000	/* 4G 首次后重同步延迟 (模组 NITZ 校准) */
#define CLK_DRIFT_SEC		30	/* 时差超过此值才更新 (防频繁跳变) */
#define CLK_VALID_TS		1735689600 /* 2025-01-01 UTC (秒) */

/* === 内部状态 === */

static int clk_synced = 0;		/* 首次同步完成标志 */
static int clk_source_4g = 0;		/* 最后一次用的是 4G (非 GPS) */
static uint64_t resync_at_ms;		/* 4G 首次后需重同步的时间点 */

/* === 工具: UTC tm → epoch (含溢出处理) === */

static time_t sysclk_tm2epoch(const struct tm *tm_utc)
{
	return timegm((struct tm *)tm_utc);
}

/* === GPS: NMEA RMC 解析 === */

/* $GNRMC,HHMMSS.SSS,A,...,DDMMYY,...  (状态 A=有定位, V=无效) */
static int gps_parse_rmc(const char *line, struct tm *tm)
{
	int hh, mm, ss, dd, mon, yy;
	char status;
	int n;

	/* 尝试 $GPRMC / $GNRMC / $BDRMC */
	if (strncmp(line, "$G", 2) != 0 && strncmp(line, "$B", 2) != 0)
		return -1;
	if (strstr(line, "RMC") == NULL)
		return -1;

	n = sscanf(line, "$%*c%*cRMC,%02d%02d%02d.%*d,%c,"
		   "%*f,%*c,%*f,%*c,%*f,%*f,%02d%02d%02d",
		   &hh, &mm, &ss, &status, &dd, &mon, &yy);
	if (n < 7 || status != 'A')
		return -1;		/* 无定位或字段不足 */

	tm->tm_year = yy + 100;		/* 26 → 2026 (1900 基准) */
	tm->tm_mon = mon - 1;
	tm->tm_mday = dd;
	tm->tm_hour = hh;
	tm->tm_min = mm;
	tm->tm_sec = ss;
	return 0;
}

/* 从串口读 NMEA, 找到含时间的 RMC 语句 */
static int gps_read_time(const char *dev, struct tm *tm)
{
	struct termios tio;
	char buf[512], line[256];
	int total = 0, fd, ret;
	struct pollfd pfd;

	fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;

	/* GPS 模块固定 9600 8N1, 只读无需配置 */
	tcgetattr(fd, &tio);
	cfmakeraw(&tio);
	cfsetispeed(&tio, B9600);
	cfsetospeed(&tio, B9600);
	tcsetattr(fd, TCSANOW, &tio);

	/* 限时读取: 绝对超时 (不能用空闲超时 — GPS 连续发 NMEA
	 * 会导致空闲计时永远重置, 循环永不退出) */
	buf[0] = '\0';
	total = 0;
	pfd.fd = fd;
	pfd.events = POLLIN;

	{
		uint64_t deadline = sys_ms() + CLK_GPS_WAIT_MS;

		while (total < (int)sizeof(buf) - 1 &&
		       sys_ms() < deadline) {
			ret = poll(&pfd, 1, 100);
			if (ret <= 0)
				continue;

			ret = read(fd, buf + total,
				   sizeof(buf) - 1 - total);
			if (ret <= 0)
				break;
			total += ret;
			buf[total] = '\0';

			/* 逐行解析 */
			{
				char *start = buf;
				char *nl;

				while ((nl = strchr(start, '\n')) != NULL) {
					int len = nl - start;

					if (len > 0 &&
					    len < (int)sizeof(line)) {
						memcpy(line, start, len);
						line[len] = '\0';
						if (line[len - 1] == '\r')
							line[len - 1] = '\0';

						if (gps_parse_rmc(line, tm) == 0) {
							close(fd);
							return 0;
						}
					}
					start = nl + 1;
				}
				/* 保留未完成的行 */
				if (start < buf + total) {
					memmove(buf, start,
						total - (start - buf));
					total -= (int)(start - buf);
					buf[total] = '\0';
				} else {
					total = 0;
					buf[0] = '\0';
				}
			}
		}
	}

	close(fd);
	return -1;
}

/* === 4G: AT+CCLK? 获取网络时间 === */

static int ec200a_read_time(const char *dev, struct tm *tm)
{
	struct termios tio;
	char cmd[16], resp[256];
	int resp_len = 0, fd, ret;
	struct pollfd pfd;

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0)
		return -1;

	tcgetattr(fd, &tio);
	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tcsetattr(fd, TCSANOW, &tio);
	tcflush(fd, TCIOFLUSH);

	/* AT+CCLK? → +CCLK: "yy/mm/dd,hh:mm:ss±zz" */
	snprintf(cmd, sizeof(cmd), "AT+CCLK?\r\n");
	if (write(fd, cmd, strlen(cmd)) != (int)strlen(cmd)) {
		close(fd);
		return -1;
	}

	/* 读响应 */
	pfd.fd = fd;
	pfd.events = POLLIN;
	resp[0] = '\0';

	while (resp_len < (int)sizeof(resp) - 1) {
		ret = poll(&pfd, 1, 500);
		if (ret <= 0) {
			/* 总超时 */
			if (resp_len > 0)
				break;	/* 有数据, 静默期 */
			break;
		}
		ret = read(fd, resp + resp_len, sizeof(resp) - 1 - resp_len);
		if (ret <= 0)
			break;
		resp_len += ret;
		resp[resp_len] = '\0';
		if (strstr(resp, "OK"))
			break;
	}

	close(fd);

	/* 解析: +CCLK: "26/11/25,12:00:00+08" */
	{
		char *p = strstr(resp, "+CCLK: \"");
		int yy, mon, dd, hh, mm, ss, tz_h;
		char tz_sign;

		if (!p)
			return -1;

		if (sscanf(p + 8, "%02d/%02d/%02d,%02d:%02d:%02d%c%02d",
			   &yy, &mon, &dd, &hh, &mm, &ss,
			   &tz_sign, &tz_h) < 8)
			return -1;

		tm->tm_year = yy + 100;		/* 26 → 2026 */
		tm->tm_mon = mon - 1;
		tm->tm_mday = dd;
		tm->tm_hour = hh;
		tm->tm_min = mm;
		tm->tm_sec = ss;

		/* 转为 UTC: 减去时区偏移 */
		if (tz_sign == '+')
			tm->tm_hour -= tz_h;
		else if (tz_sign == '-')
			tm->tm_hour += tz_h;
		/* mktime/timegm 处理溢出 */

		return 0;
	}
}

/* === 同步入口 === */

/* 设置系统时钟 (仅当与当前差 > CLK_DRIFT_SEC 时更新) */
static void sysclk_set_if_drifted(const struct tm *tm_utc)
{
	struct timeval cur, new_tv;
	time_t new_t = sysclk_tm2epoch(tm_utc);

	if (new_t <= 0)
		return;

	gettimeofday(&cur, NULL);
	if (cur.tv_sec > CLK_VALID_TS &&
	    labs(cur.tv_sec - new_t) < CLK_DRIFT_SEC)
		return;			/* 时差小, 不跳变 */

	new_tv.tv_sec = new_t;
	new_tv.tv_usec = 0;
	settimeofday(&new_tv, NULL);
}

static void clk_sync(void)
{
	const char *gps_dev = conf_get("gps_dev", "/dev/gps");
	const char *at_dev = conf_get("at_dev", "/dev/ttyUSB2");
	struct tm tm;
	int ret;

	/* 1. GPS 优先 */
	memset(&tm, 0, sizeof(tm));
	ret = gps_read_time(gps_dev, &tm);
	if (ret == 0) {
		sysclk_set_if_drifted(&tm);
		clk_synced = 1;
		clk_source_4g = 0;
		resync_at_ms = 0;
		printf("sysclk: GPS 时钟已同步 "
		       "%04d-%02d-%02d %02d:%02d:%02d UTC\n",
		       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		       tm.tm_hour, tm.tm_min, tm.tm_sec);
		return;
	}

	/* 2. 4G 兜底 */
	memset(&tm, 0, sizeof(tm));
	ret = ec200a_read_time(at_dev, &tm);
	if (ret == 0) {
		sysclk_set_if_drifted(&tm);
		clk_synced = 1;
		clk_source_4g = 1;
		/* 4G 模组刚上电时 RTC 可能还没被网络 NITZ 校准
		 * (时间偏差可达数天), 30 秒后重同步一次 */
		resync_at_ms = sys_ms() + CLK_RESYNC_DELAY_MS;
		printf("sysclk: 4G 网络时钟已同步 "
		       "%04d-%02d-%02d %02d:%02d:%02d UTC\n",
		       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		       tm.tm_hour, tm.tm_min, tm.tm_sec);
		return;
	}

	/* 都失败 */
	if (!clk_synced)
		printf("sysclk: GPS 无定位, 4G 时钟不可用, 同步失败\n");
}

/* === initcall: 启动时同步 (保证 TLS 前时间正确) === */

static int clk_sysinit(void)
{
	/* 无 RTC 板子每次重启时钟回到内核构建时间 (可能"看起来
	 * 有效"但早于 TLS 证书 notBefore), 始终尝试同步 */
	clk_sync();
	return 0;			/* 同步失败不阻塞启动 */
}
initcall_core(clk_sysinit);

/* === pumpcall: 周期检查 === */

static void clk_syspump(void)
{
	static uint64_t next_retry_ms;
	uint64_t now = sys_ms();

	/* 4G 首次同步后 30s 重同步 (模组 NITZ 校准后时间才准确) */
	if (resync_at_ms > 0 && now >= resync_at_ms) {
		resync_at_ms = 0;
		printf("sysclk: 4G 时钟重同步 (模组应已完成 NITZ 校准)\n");
		clk_sync();
		return;
	}

	/* 从未同步成功: 每 60s 重试 */
	if (!clk_synced) {
		if (now < next_retry_ms)
			return;
		next_retry_ms = now + CLK_RETRY_MS;
		clk_sync();
		return;
	}

	/* 已同步且用 GPS: 无需后续 (GPS 是原子钟) */
	if (!clk_source_4g)
		return;

	/* 用 4G: 每 300s 校验一次 (4G 模组时钟可能漂移) */
	{
		static uint64_t next_check_ms;

		if (now < next_check_ms)
			return;
		next_check_ms = now + 300000;

		/* 只有时差大才真正重设 */
		clk_sync();
	}
}
pumpcall(clk_syspump);
