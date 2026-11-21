/*
 * sal/initcall.c - initcall/pumpcall 机制实现
 *
 * 段边界符号由 initcall.lds 定义 (INSERT AFTER 增补默认链接脚本):
 *   __ic_start / __ic_l{N} / __ic_stop : 分层 initcall 段
 *   __pc_start / __pc_stop             : pumpcall 段
 */
#include <sal/initcall.h>

#include <stdio.h>

extern const struct initcall_entry __ic_start[];
extern const struct initcall_entry __ic_l0[];
extern const struct initcall_entry __ic_l1[];
extern const struct initcall_entry __ic_l2[];
extern const struct initcall_entry __ic_l3[];
extern const struct initcall_entry __ic_l4[];
extern const struct initcall_entry __ic_l5[];
extern const struct initcall_entry __ic_stop[];

extern const struct pumpcall_entry __pc_start[];
extern const struct pumpcall_entry __pc_stop[];

/* 每层的起止 (层 N 的区间的终点用层 N+1 的起点, 最后一层用 __ic_stop) */
static const struct initcall_entry *const level_start[INIT_LEVEL_COUNT + 1] = {
	__ic_l0, __ic_l1, __ic_l2, __ic_l3, __ic_l4, __ic_l5, __ic_stop
};

static int level_done[INIT_LEVEL_COUNT];

int initcall_count(void)
{
	return (int)(__ic_stop - __ic_start);
}

int initcalls_run_level(int level)
{
	const struct initcall_entry *e, *end;
	int fails = 0;

	if (level < 0 || level >= INIT_LEVEL_COUNT)
		return -1;
	if (level_done[level])
		return 0;
	level_done[level] = 1;

	end = level_start[level + 1];
	for (e = level_start[level]; e < end; e++) {
		int r = e->fn();

		if (r != 0) {
			printf("[init %d] %-24s FAIL (%d)\n",
			       level, e->name, r);
			fails++;
		} else {
			printf("[init %d] %-24s ok\n", level, e->name);
		}
	}
	return fails;
}

int initcalls_run(void)
{
	int lvl, fails = 0;

	for (lvl = 0; lvl < INIT_LEVEL_COUNT; lvl++)
		fails += initcalls_run_level(lvl);
	return fails;
}

void initcalls_dump(void)
{
	int lvl;

	printf("initcalls (%d):\n", initcall_count());
	for (lvl = 0; lvl < INIT_LEVEL_COUNT; lvl++) {
		const struct initcall_entry *e;

		for (e = level_start[lvl]; e < level_start[lvl + 1]; e++)
			printf("  [%d] %s\n", lvl, e->name);
	}
}

void pumpcalls_run(void)
{
	const struct pumpcall_entry *e;

	for (e = __pc_start; e < __pc_stop; e++)
		e->fn();
}

int pumpcall_count(void)
{
	return (int)(__pc_stop - __pc_start);
}
