/*
 * include/pal/sysio.h - 系统操作层: 子进程/mtd/cmdline/时钟/序列号
 */
#ifndef PAL_SYSIO_H
#define PAL_SYSIO_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* === 子进程执行 (fork + exec + 轮询 waitpid) === */

#define PROC_IDLE	0
#define PROC_RUNNING	1
#define PROC_DONE	2

struct child_proc {
	pid_t pid;
	int state;
	int exit_code;		/* PROC_DONE 时的退出码 */
};

/* 启动子进程 (argv 以 NULL 结尾)。返回 0 成功。 */
int sys_run(struct child_proc *p, const char *const argv[]);

/* 轮询: 若结束返回 0 并填充 exit_code; 仍在运行返回 1; 错误 -1。 */
int sys_poll(struct child_proc *p);

/* 杀掉并回收 (不用时清理)。 */
void sys_kill(struct child_proc *p);

/* 同步执行: 阻塞直到结束, 返回退出码 (<0 启动失败)。 */
int sys_run_sync(const char *const argv[]);

/* === /proc/mtd 按名查设备 === */

/* 找到分区对应的 /dev/mtdN。返回如 "mtd5" 的静态指针或 NULL。 */
const char *sys_mtd_by_name(const char *part_name);

/* === /proc/cmdline 解析 === */

/* 取内核启动参数 (如 "ab_slot")。找到返回 0 并写入 val。 */
int sys_cmdline_get(const char *key, char *val, int val_cap);

/* === 单调毫秒时钟 (进程内, CLOCK_MONOTONIC) === */
uint64_t sys_ms(void);

/* === 毫秒序列号 (严格递增, 跨毫秒也不重复) === */
uint64_t sys_seq(void);

/* === 大端读写 (协议报文) === */
uint64_t be64_get(const uint8_t *p);
void be64_put(uint8_t *p, uint64_t v);
uint32_t be32_get(const uint8_t *p);
void be32_put(uint8_t *p, uint32_t v);
uint16_t be16_get(const uint8_t *p);
void be16_put(uint8_t *p, uint16_t v);

/* === 文件操作 === */

/* 文件大小, 失败 -1 */
long long sys_file_size(const char *path);

/* 读整个文件到调用方缓冲区。返回读到的字节数, 失败 -1。 */
int sys_read_file(const char *path, uint8_t *buf, int cap);

/* 删除文件 (不存在不算错) */
void sys_unlink(const char *path);

#endif /* PAL_SYSIO_H */
