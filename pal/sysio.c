/*
 * pal/sysio.c - 系统操作层实现
 */
#include <pal/sysio.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

/* === 子进程 === */

int sys_run(struct child_proc *p, const char *const argv[])
{
	pid_t pid;

	memset(p, 0, sizeof(*p));

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		/* 子进程: 关闭不需要的描述符 */
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}

	p->pid = pid;
	p->state = PROC_RUNNING;
	return 0;
}

int sys_poll(struct child_proc *p)
{
	int status;
	pid_t r;

	if (p->state != PROC_RUNNING)
		return 0;

	r = waitpid(p->pid, &status, WNOHANG);
	if (r == 0)
		return 1;		/* 仍在运行 */

	if (r < 0) {
		if (errno == EINTR)
			return 1;
		p->state = PROC_DONE;
		p->exit_code = -1;
		return 0;
	}

	p->state = PROC_DONE;
	if (WIFEXITED(status))
		p->exit_code = WEXITSTATUS(status);
	else
		p->exit_code = -1;
	return 0;
}

void sys_kill(struct child_proc *p)
{
	if (p->state == PROC_RUNNING) {
		kill(p->pid, SIGKILL);
		waitpid(p->pid, NULL, 0);
	}
	p->state = PROC_IDLE;
}

int sys_run_sync(const char *const argv[])
{
	struct child_proc p;
	int timer = 0;

	if (sys_run(&p, argv) != 0)
		return -1;

	while (sys_poll(&p) == 1) {
		usleep(20000);
		if (++timer > 3000) {	/* 60 秒保险 */
			sys_kill(&p);
			return -1;
		}
	}
	return p.exit_code;
}

/* === /proc/mtd 按名查设备 === */

const char *sys_mtd_by_name(const char *part_name)
{
	static char devname[16];
	static char lastname[64];
	FILE *fp;
	char line[128];

	/* 简单缓存: 同名重复查询直接返回 */
	if (lastname[0] && strcmp(lastname, part_name) == 0)
		return devname;

	fp = fopen("/proc/mtd", "r");
	if (!fp)
		return NULL;

	while (fgets(line, sizeof(line), fp)) {
		char name[64];

		if (sscanf(line, "%15[^:]: %*s \"%63[^\"]\"", devname, name) == 2) {
			if (strcmp(name, part_name) == 0) {
				fclose(fp);
				strncpy(lastname, part_name, sizeof(lastname) - 1);
				return devname;		/* "mtd5" */
			}
		}
	}
	fclose(fp);
	return NULL;
}

/* === /proc/cmdline === */

int sys_cmdline_get(const char *key, char *val, int val_cap)
{
	char buf[1024];
	int fd, n;
	char *p, *end;
	size_t klen = strlen(key);

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';

	p = buf;
	while ((p = strstr(p, key)) != NULL) {
		if ((p == buf || p[-1] == ' ') && p[klen] == '=') {
			p += klen + 1;
			end = strchr(p, ' ');
			if (!end)
				end = p + strlen(p);
			if (end - p >= val_cap)
				end = p + val_cap - 1;
			memcpy(val, p, end - p);
			val[end - p] = '\0';
			return 0;
		}
		p++;
	}
	return -1;
}

/* === 时钟 === */

uint64_t sys_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t sys_seq(void)
{
	static uint64_t last;
	static int inited;
	uint64_t now;

	if (!inited) {
		/* 基准: 挂钟毫秒 (无 RTC 时不可靠, 但保证进程内严格递增) */
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		last = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
		inited = 1;
	}

	now = sys_ms();			/* 单调基准 */
	now = now > last ? now : last + 1;
	last = now;
	return now;
}

/* === 大端读写 === */

uint64_t be64_get(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

void be64_put(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (56 - 8 * i));
}

uint32_t be32_get(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

void be32_put(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

uint16_t be16_get(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

void be16_put(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* === 文件 === */

long long sys_file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	return (long long)st.st_size;
}

int sys_read_file(const char *path, uint8_t *buf, int cap)
{
	int fd, n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, cap);
	close(fd);
	return n;
}

void sys_unlink(const char *path)
{
	unlink(path);
}
