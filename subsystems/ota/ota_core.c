/*
 * subsystems/ota/ota_core.c - OTA 升级子系统 (协议 V3.0 6.5 / 8.x)
 *
 * 状态机:
 *   IDLE -> DL_RUNNING(下载+进度上报) -> VERIFY(SM3+SM2)
 *   -> INSTALL(擦写非活动槽+回读) -> SWITCH(fw_setenv 切槽)
 *   -> REBOOT -> (新固件启动后) PENDING_ACTIVATION
 *   -> 激活(stable=1) 或 10min 超时回退
 *
 * 安全保证:
 *   - 全程只碰非活动槽, 任何失败当前系统无损
 *   - U-Boot try 计数兜底启动失败 (3 次自动回退)
 *   - 10 分钟未激活 APP 自动回退 (协议 6.5 第 5 步)
 */
#include <subsystems/ota.h>
#include <net/net.h>
#include <app/proto.h>
#include <app/online.h>
#include <net/https.h>
#include <common/sm3.h>
#include <common/sm2.h>
#include <pal/config.h>
#include <pal/sysio.h>
#include <sal/initcall.h>
#include <sal/evbus.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>

/* === 平台 SM2 公钥 ===
 * 编译时默认值 = 测试服务器密钥对 (ota_server 首次运行生成)。
 * 生产时替换为正式公钥，或通过 hydg02.conf 的 sm2_pubkey= 覆盖。
 * 格式: 128 字符 hex (64 字节 x||y 大端)。
 */
static uint8_t platform_pubkey[64] = {
	0x51, 0x8d, 0x75, 0x6d, 0x94, 0xf5, 0x25, 0x9e,
	0x1c, 0xd2, 0xca, 0x67, 0x9a, 0x25, 0xe7, 0x11,
	0x34, 0x30, 0xb3, 0x30, 0xf1, 0xb6, 0xe9, 0x82,
	0xe5, 0x52, 0x1c, 0x8b, 0x9d, 0x2d, 0xed, 0x2b,
	0x17, 0xca, 0xd4, 0xd3, 0x3f, 0x74, 0x64, 0x52,
	0x17, 0xee, 0xa1, 0x4a, 0x9a, 0x15, 0x7d, 0x86,
	0xbf, 0x93, 0xa0, 0x2c, 0xce, 0xba, 0x65, 0xc2,
	0x64, 0x77, 0xd2, 0x7f, 0xc8, 0xb2, 0xfc, 0xc6,
};

/* === 状态 === */

enum ota_state {
	OTA_IDLE = 0,
	OTA_DL,				/* 下载中 */
	OTA_VERIFY,			/* SM3+SM2 校验 (整包) */
	OTA_EXTRACT,			/* 解压 + manifest + 版本校验 + 提取固件 */
	OTA_INSTALL,			/* 擦写非活动槽 */
	OTA_SWITCH,			/* env 切槽 + 重启 */
	OTA_PENDING_ACT,		/* 新固件启动, 等激活 */
};

static enum ota_state st = OTA_IDLE;
static uint64_t cur_seq;			/* 0x04 序列号 */
static uint8_t cur_file_type;
static char cur_url[256];
static char cur_ver[32];
static uint8_t cur_sm3[32];
static uint8_t cur_sig[64];
static int has_sig;

static uint64_t act_deadline_ms;			/* 10min 激活超时 */
static char act_target_ver[32];			/* 待激活版本 */
static char act_from_slot;				/* 回退方向 */

#define OTA_TMP_FILE	"/tmp/hydg02-upgrade.bin"
#define OTA_EXTRACT_DIR	"/tmp/ota_pkg"
#define OTA_FW_BIN	"/tmp/ota_pkg/firmware.bin"
#define OTA_MANIFEST	"/tmp/ota_pkg/manifest.txt"
#define OTA_SLOT_SIZE	0x450000			/* 4.3MB 槽上限 */
#define ACT_TIMEOUT_MS	(10 * 60 * 1000)			/* 10 分钟 */

/* === 进度上报 (0x84) === */

static void report(uint8_t phase, uint8_t progress, uint8_t rc)
{
	proto_send_progress(cur_seq, cur_file_type, phase, progress, rc);
}

/* === AB 槽操作 === */

/* 当前活动槽: /proc/cmdline ab_slot=A/B */
static char get_active_slot(void)
{
	char v[8];

	if (sys_cmdline_get("ab_slot", v, sizeof(v)) == 0)
		return v[0];
	return 0;
}

/* 非活动槽的 mtd 分区名 */
static const char *get_inactive_mtd(void)
{
	char slot = get_active_slot();

	if (slot == 'A')
		return "firmware_b";
	if (slot == 'B')
		return "firmware_a";
	return NULL;
}

/* fw_setenv 封装 */
static int fw_set(const char *key, const char *val)
{
	char *argv[] = { "/usr/sbin/fw_setenv",
			 (char *)key, (char *)val, NULL };

	return sys_run_sync((const char *const *)argv);
}

static int fw_get(const char *key, char *val, int cap)
{
	/* 简化: fw_printenv key */
	char *argv[] = { "/usr/sbin/fw_printenv",
			 (char *)key, NULL };
	FILE *fp;
	char cmd[128], line[128];

	(void)argv;
	snprintf(cmd, sizeof(cmd), "/usr/sbin/fw_printenv %s 2>/dev/null", key);
	fp = popen(cmd, "r");
	if (!fp)
		return -1;
	if (fgets(line, sizeof(line), fp)) {
		char *eq = strchr(line, '=');

		if (eq) {
			char *p = eq + 1;
			char *end;
			int n;

			/* 去尾部空白 (fw_printenv 输出带 \n,
			 * 残留会导致版本 strcmp 永远不匹配) */
			end = p + strlen(p);
			while (end > p && (end[-1] == '\n' ||
					   end[-1] == '\r' ||
					   end[-1] == ' ' ||
					   end[-1] == '\t'))
				*--end = '\0';

			n = strlen(p);
			if (n >= cap) n = cap - 1;
			memcpy(val, p, n);
			val[n] = '\0';
			pclose(fp);
			return 0;
		}
	}
	pclose(fp);
	return -1;
}

/* === 下载进度回调 (HTTPS -> 0x84) === */

static int dl_progress(void *ctx, uint64_t recv, uint64_t total)
{
	uint8_t pct = 0;

	(void)ctx;
	if (total > 0)
		pct = (uint8_t)(recv * 50 / total);	/* 下载占 0-50% */

	/* 1% 变化或每秒上报 */
	static uint8_t last_pct;
	static uint64_t last_ms;

	uint64_t now = sys_ms();

	if (pct != last_pct || now - last_ms > 1000) {
		report(PROTO_PHASE_DOWNLOAD, pct, PROTO_RC_OK);
		last_pct = pct;
		last_ms = now;
	}
	return 0;
}

/* === 状态机 pump === */

void ota_pump(void)
{
	switch (st) {
	case OTA_IDLE:
		break;

	case OTA_DL: {
		/* 同步下载 (在 pump 里阻塞, 下载完成后切状态) */
		int ret = https_get_file(cur_url, OTA_TMP_FILE,
					 conf_get("ca_cert", ""),
					 dl_progress, NULL);

		if (ret != 200) {
			printf("ota: 下载失败 (%d)\n", ret);
			report(PROTO_PHASE_FAIL, 0, PROTO_RC_DOWNLOAD);
			sys_unlink(OTA_TMP_FILE);
			st = OTA_IDLE;
			return;
		}
		printf("ota: 下载完成\n");
		st = OTA_VERIFY;
		break;
	}

	case OTA_VERIFY: {
		uint8_t digest[32];

		report(PROTO_PHASE_VERIFY, 0, PROTO_RC_OK);

		/* 尺寸检查 (整包, 含压缩开销) */
		{
			long long sz = sys_file_size(OTA_TMP_FILE);

			if (sz <= 0 || sz > OTA_SLOT_SIZE + 65536) {
				printf("ota: 包尺寸异常 (%lld)\n", sz);
				report(PROTO_PHASE_FAIL, 0, PROTO_RC_PARAM);
				sys_unlink(OTA_TMP_FILE);
				st = OTA_IDLE;
				return;
			}
		}

		/* SM3 校验 (对整包) */
		if (sm3_file(OTA_TMP_FILE, digest) != 0) {
			report(PROTO_PHASE_FAIL, 0, PROTO_RC_OTHER);
			sys_unlink(OTA_TMP_FILE);
			st = OTA_IDLE;
			return;
		}
		if (memcmp(digest, cur_sm3, 32) != 0) {
			printf("ota: SM3 校验失败\n");
			report(PROTO_PHASE_FAIL, 0, PROTO_RC_CHECKSUM);
			sys_unlink(OTA_TMP_FILE);
			st = OTA_IDLE;
			return;
		}
		printf("ota: SM3 校验通过\n");

		/* SM2 验签 (对 SM3 摘要验签) */
		if (has_sig) {
			if (sm2_verify(platform_pubkey, digest, cur_sig) != 1) {
				printf("ota: SM2 验签失败\n");
				report(PROTO_PHASE_FAIL, 0, PROTO_RC_CHECKSUM);
				sys_unlink(OTA_TMP_FILE);
				st = OTA_IDLE;
				return;
			}
			printf("ota: SM2 验签通过\n");
		} else if (cur_file_type != PROTO_FILE_MP3) {
			printf("ota: 固件缺少 SM2 签名\n");
			report(PROTO_PHASE_FAIL, 0, PROTO_RC_PARAM);
			sys_unlink(OTA_TMP_FILE);
			st = OTA_IDLE;
			return;
		}

		st = OTA_EXTRACT;
		break;
	}

	case OTA_EXTRACT: {
		/* 解压升级包, 读 manifest, 校验版本, 提取固件 */

		/* 清理解压目录 */
		{
			char *argv[] = { "/bin/rm", "-rf",
					 OTA_EXTRACT_DIR, NULL };

			sys_run_sync((const char *const *)argv);
		}
		mkdir(OTA_EXTRACT_DIR, 0755);

		/* 解压 (tar 处理 .tar.gz/.tgz, unzip 处理 .zip) */
		{
			int ret;
			const char *suffix = strrchr(cur_url, '.');

			if (suffix && (strstr(cur_url, ".tar.gz") ||
				       strstr(cur_url, ".tgz"))) {
				char *argv[] = { "/bin/tar", "xzf",
						 OTA_TMP_FILE, "-C",
						 OTA_EXTRACT_DIR, NULL };
				ret = sys_run_sync(
					(const char *const *)argv);
			} else if (suffix && !strcmp(suffix, ".zip")) {
				char *argv[] = { "/usr/bin/unzip",
						 "-o", "-q",
						 OTA_TMP_FILE, "-d",
						 OTA_EXTRACT_DIR, NULL };
				ret = sys_run_sync(
					(const char *const *)argv);
			} else {
				/* 无压缩后缀: 直接当固件用 */
				printf("ota: 无压缩后缀, 直接安装\n");
				ret = 0;
				{
					char *argv[] = { "/bin/cp",
							 OTA_TMP_FILE,
							 OTA_FW_BIN,
							 NULL };
					sys_run_sync(
						(const char *const *)argv);
				}
			}

			if (ret != 0) {
				printf("ota: 解压失败\n");
				report(PROTO_PHASE_FAIL, 0,
				       PROTO_RC_PARAM);
				sys_unlink(OTA_TMP_FILE);
				st = OTA_IDLE;
				return;
			}
		}
		printf("ota: 解压完成\n");

		/* 读 manifest + 版本校验 */
		{
			const char *fw_ver;
			const char *run_ver;
			char fw_ver_buf[32];

			if (access(OTA_MANIFEST, F_OK) == 0) {
				conf_load(OTA_MANIFEST);
				fw_ver = conf_get("version", "");
				printf("ota: manifest version=%s\n",
				       fw_ver);
			} else {
				/* 无 manifest: 用 0x04 里的目标版本 */
				strncpy(fw_ver_buf, cur_ver,
					sizeof(fw_ver_buf) - 1);
				fw_ver_buf[sizeof(fw_ver_buf) - 1] = '\0';
				fw_ver = fw_ver_buf;
				printf("ota: 无 manifest, 用 0x04 版本=%s\n",
				       fw_ver);
			}

			/* 版本校验: 大版本必须增加 */
			run_ver = conf_get("fw_version", "0.0.0");
			{
				int run_major = 0, fw_major = 0;

				sscanf(run_ver, "%d", &run_major);
				sscanf(fw_ver, "%d", &fw_major);

				printf("ota: 版本校验 当前=%d 新=%d\n",
				       run_major, fw_major);
				if (fw_major <= run_major) {
					printf("ota: 大版本未增加, 拒绝升级\n");
					report(PROTO_PHASE_FAIL, 0,
					       PROTO_RC_PARAM);
					sys_unlink(OTA_TMP_FILE);
					st = OTA_IDLE;
					return;
				}
				printf("ota: 版本校验通过\n");
			}
		}

		/* 检查固件二进制存在 + uImage 魔数 */
		{
			long long sz = sys_file_size(OTA_FW_BIN);
			uint8_t hdr[4];
			int fd;

			if (sz <= 0 || sz > OTA_SLOT_SIZE) {
				printf("ota: 固件尺寸异常 (%lld)\n", sz);
				report(PROTO_PHASE_FAIL, 0,
				       PROTO_RC_PARAM);
				sys_unlink(OTA_TMP_FILE);
				st = OTA_IDLE;
				return;
			}

			fd = open(OTA_FW_BIN, O_RDONLY);
			if (fd < 0 || read(fd, hdr, 4) != 4) {
				if (fd >= 0) close(fd);
				report(PROTO_PHASE_FAIL, 0,
				       PROTO_RC_PARAM);
				sys_unlink(OTA_TMP_FILE);
				st = OTA_IDLE;
				return;
			}
			close(fd);
			if (hdr[0] != 0x27 || hdr[1] != 0x05 ||
			    hdr[2] != 0x19 || hdr[3] != 0x56) {
				printf("ota: uImage 魔数错误\n");
				report(PROTO_PHASE_FAIL, 0,
				       PROTO_RC_PARAM);
				sys_unlink(OTA_TMP_FILE);
				st = OTA_IDLE;
				return;
			}
			printf("ota: 固件校验通过 (%lld bytes)\n", sz);
		}

		st = OTA_INSTALL;
		break;
	}

	case OTA_INSTALL: {
		/* 写入非活动槽: mtd erase + mtd write (同步) */
		const char *mtd = get_inactive_mtd();
		char *erase_argv[4], *write_argv[5];

		if (!mtd) {
			printf("ota: 无法确定非活动槽\n");
			report(PROTO_PHASE_FAIL, 0, PROTO_RC_OTHER);
			st = OTA_IDLE;
			return;
		}

		report(PROTO_PHASE_INSTALL, 55, PROTO_RC_OK);

		erase_argv[0] = "/sbin/mtd";
		erase_argv[1] = "erase";
		erase_argv[2] = (char *)mtd;
		erase_argv[3] = NULL;
		if (sys_run_sync((const char *const *)erase_argv) != 0) {
			printf("ota: mtd erase 失败\n");
			report(PROTO_PHASE_FAIL, 0, PROTO_RC_OTHER);
			st = OTA_IDLE;
			return;
		}
		printf("ota: 槽 %s 已擦除\n", mtd);

		report(PROTO_PHASE_INSTALL, 70, PROTO_RC_OK);

		/* 写入提取后的固件二进制 (非压缩包) */
		write_argv[0] = "/sbin/mtd";
		write_argv[1] = "write";
		write_argv[2] = OTA_FW_BIN;
		write_argv[3] = (char *)mtd;
		write_argv[4] = NULL;
		if (sys_run_sync((const char *const *)write_argv) != 0) {
			printf("ota: mtd write 失败\n");
			report(PROTO_PHASE_FAIL, 0, PROTO_RC_OTHER);
			st = OTA_IDLE;
			return;
		}
		printf("ota: 固件已写入槽 %s\n", mtd);

		report(PROTO_PHASE_INSTALL, 95, PROTO_RC_OK);
		st = OTA_SWITCH;
		break;
	}

	case OTA_SWITCH: {
		char other = (get_active_slot() == 'A') ? 'B' : 'A';
		char slot_str[2] = { other, '\0' };
		char stable_key[20], try_key[18];

		/* 协议 6.5: 写入备份分区后切槽重启 */
		fw_set("boot_slot", slot_str);
		snprintf(stable_key, sizeof(stable_key),
			 "slot_%c_stable", other);
		snprintf(try_key, sizeof(try_key), "slot_%c_try", other);
		fw_set(stable_key, "0");
		fw_set(try_key, "0");

		/* OTA 待激活状态 (env 变量, 跨重启) */
		fw_set("ota_pending", "1");
		fw_set("ota_ver", cur_ver);
		fw_set("ota_seq", "");	/* seq 无法精确转字符串, 用 0 */
		{
			char seq_str[24];

			snprintf(seq_str, sizeof(seq_str), "%llu",
				 (unsigned long long)cur_seq);
			fw_set("ota_seq", seq_str);
		}
		fw_set("ota_from", (char[]){ get_active_slot(), '\0' });

		report(PROTO_PHASE_REBOOT, 100, PROTO_RC_OK);
		printf("ota: 切槽到 %c, 2 秒后重启\n", other);

		st = OTA_IDLE;
		sleep(2);			/* 等 0x84 发出 */
		sync();
		{
			char *argv[] = { "/sbin/reboot", NULL };

			sys_run_sync((const char *const *)argv);
		}
		break;
	}

	case OTA_PENDING_ACT: {
		/* 新固件启动后: 等激活或 10 分钟超时回退 */
		uint64_t now = sys_ms();

		if (now >= act_deadline_ms) {
			char from_str[2] = { act_from_slot, '\0' };

			printf("ota: 10 分钟未激活, 回退到槽 %c\n",
			       act_from_slot);
			fw_set("boot_slot", from_str);
			fw_set("ota_pending", "0");
			sync();
			{
				char *argv[] = { "/sbin/reboot", NULL };

				sys_run_sync((const char *const *)argv);
			}
		}
		break;
	}
	}
}

/* === 启动时检查待激活状态 === */

static int ota_check_pending(void)
{
	char val[32];

	if (fw_get("ota_pending", val, sizeof(val)) != 0)
		return 0;
	if (val[0] != '1')
		return 0;

	if (fw_get("ota_ver", act_target_ver, sizeof(act_target_ver)) != 0)
		act_target_ver[0] = '\0';
	if (fw_get("ota_from", val, sizeof(val)) == 0 && val[0])
		act_from_slot = val[0];
	else
		act_from_slot = (get_active_slot() == 'A') ? 'B' : 'A';

	/* 恢复 0x04 序列号 (用于 0x84 成功上报) */
	if (fw_get("ota_seq", val, sizeof(val)) == 0 && val[0])
		cur_seq = strtoull(val, NULL, 10);
	cur_file_type = PROTO_FILE_FW;

	st = OTA_PENDING_ACT;
	act_deadline_ms = sys_ms() + ACT_TIMEOUT_MS;
	printf("ota: 待激活状态 (ver=%s, %ld 分钟超时)\n",
	       act_target_ver, (long)(ACT_TIMEOUT_MS / 60000));
	return 1;
}

/* === 激活 (proto 0x02/0x05/0x03) === */

int ota_activate(const char *version)
{
	char slot = get_active_slot();
	char stable_key[20], try_key[18];

	if (st != OTA_PENDING_ACT)
		return -1;

	/* 协议 6.5 第 5 步: 校验当前运行版本与目标一致后激活 */
	if (strcmp(version, act_target_ver) != 0) {
		printf("ota: 激活版本不匹配 (cur=%s want=%s)\n",
		       act_target_ver, version);
		return -1;
	}

	snprintf(stable_key, sizeof(stable_key), "slot_%c_stable", slot);
	snprintf(try_key, sizeof(try_key), "slot_%c_try", slot);
	fw_set(stable_key, "1");
	fw_set(try_key, "0");
	fw_set("ota_pending", "0");

	st = OTA_IDLE;
	printf("ota: 槽 %c 已激活 (stable=1)\n", slot);

	/* 上报 0x84 成功 (带回 0x04 序列号) */
	report(PROTO_PHASE_SUCCESS, 100, PROTO_RC_OK);
	return 0;
}

/* === 对外接口 === */

int ota_start(uint64_t seq, uint8_t file_type,
	      const char *url, const char *ver,
	      const uint8_t *sm3, const uint8_t *sm2_sig)
{
	if (st != OTA_IDLE)
		return -1;			/* 忙 */
	if (!url || !url[0] || !sm3)
		return -1;

	cur_seq = seq;
	cur_file_type = file_type;
	strncpy(cur_url, url, sizeof(cur_url) - 1);
	strncpy(cur_ver, ver ? ver : "", sizeof(cur_ver) - 1);
	memcpy(cur_sm3, sm3, 32);
	if (sm2_sig) {
		memcpy(cur_sig, sm2_sig, 64);
		has_sig = 1;
	} else {
		has_sig = 0;
	}

	st = OTA_DL;

	/* 0x84 已接收 */
	report(PROTO_PHASE_RECVED, 0, PROTO_RC_OK);
	printf("ota: 开始下载 %s\n", url);
	return 0;
}

int ota_busy(void)
{
	return st != OTA_IDLE;
}

/* === initcall + pumpcall === */

static int ota_sysinit(void)
{
	/* fw_env.config 自检补建 (独立部署模式) */
	if (access("/etc/fw_env.config", F_OK) != 0) {
		const char *mtd = sys_mtd_by_name("u-boot-env");
		FILE *fp;

		if (mtd) {
			fp = fopen("/etc/fw_env.config", "w");
			if (fp) {
				fprintf(fp, "/dev/%s 0x00000 0x01000 0x10000\n",
					mtd);
				fclose(fp);
				printf("ota: fw_env.config 已生成 (/dev/%s)\n",
				       mtd);
			}
		}
	}

	/* SM2 公钥: 配置文件覆盖 (128 字符 hex) */
	{
		const char *hex = conf_get("sm2_pubkey", "");

		if (strlen(hex) == 128) {
			int i, ok = 1;

			for (i = 0; i < 64; i++) {
				unsigned int b;

				if (sscanf(hex + i * 2, "%2x", &b) != 1) {
					ok = 0;
					break;
				}
				platform_pubkey[i] = (uint8_t)b;
			}
			if (ok)
				printf("ota: SM2 公钥已从配置加载\n");
			else
				printf("ota: SM2 公钥配置格式错误, 用默认值\n");
		}
	}

	/* 检查跨重启的待激活状态 */
	ota_check_pending();
	return 0;
}
initcall_core(ota_sysinit);

static void ota_syspump(void)
{
	ota_pump();
}
pumpcall(ota_syspump);
