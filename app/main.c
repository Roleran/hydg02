/*
 * app/main.c - Application entry point
 *
 * 架构 (Linux 内核风格 initcall):
 *   - 各模块的 init 经 initcall_xxx() 宏注册进链接段 .initcalls
 *   - main 只调用一次 initcalls_run(): EARLY->SAL->DRIVER->CORE->APP->LATE
 *   - 主循环只调用 pumpcalls_run(): 各模块的 pump 钩子同样入段注册
 *
 * 本文件不包含任何模块细节 —— 新增子系统时只需在模块内注册 initcall,
 * main.c 零改动。
 */
#include <sal/initcall.h>
#include <sal/evbus.h>
#include <sal/wq.h>
#include <pal/config.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#define APP_NAME "hydg02"
#define APP_VERSION "0.3.0-initcall"

static volatile int running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

static void print_banner(void)
{
	printf("%s v%s - subsystem framework (initcall)\n",
	       APP_NAME, APP_VERSION);
	printf("initcalls=%d pumpcalls=%d\n\n",
	       initcall_count(), pumpcall_count());
}

/* === Self-test: verify framework primitives === */

static int selftest_evbus(void)
{
	struct evbus bus;

	evbus_init(&bus);
	/* verify init produces clean state */
	if (bus.n_subs != 0)
		return 0;
	return 1;
}

static int selftest_wq(void)
{
	struct wq q;
	static int dummy;

	wq_init(&q);
	if (q.n_items != 0)
		return 0;
	{
		int idx = wq_register(&q, (wq_fn_t)&dummy, NULL);

		if (idx != 0)
			return 0;
		wq_schedule(&q, idx, 100);
		if (q.items[idx].deadline_ms == 0)
			return 0;
		wq_cancel(&q, idx);
		if (q.items[idx].deadline_ms != 0)
			return 0;
	}
	return 1;
}

/* initcall 集成测试: 全层级执行 + pump 不崩溃 + 计数合理 */
static int selftest_initcall(void)
{
	if (initcalls_run() != 0)
		return 0;		/* 任一 initcall 失败 */
	if (initcall_count() < 5)
		return 0;		/* conf+evbus+wq+ec200a+net+app */
	pumpcalls_run();
	pumpcalls_run();
	return 1;
}

static int selftest(void)
{
	int pass = 0, total = 3;

	if (selftest_evbus())
		pass++;
	else
		printf("evbus: FAIL\n");

	if (selftest_wq())
		pass++;
	else
		printf("wq: FAIL\n");

	if (selftest_initcall())
		pass++;
	else
		printf("initcall: FAIL\n");

	printf("selftest: %d/%d pass\n", pass, total);
	return pass == total ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
		return selftest();

	print_banner();

	/* setup signal handling */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* 配置路径 (先于 initcalls_run, conf_sysinit 读取) */
	if (argc > 1)
		g_conf_path = argv[1];

	/* 分层初始化全部子系统 */
	if (initcalls_run() > 0)
		printf("WARNING: some initcalls failed, degraded mode\n");

	printf("\nFramework ready. Entering main loop...\n");

	/* === Main pump loop (钩子由各模块 pumpcall 注册) === */
	while (running) {
		pumpcalls_run();
		usleep(10000);
	}

	printf("\nShutting down...\n");
	printf("Done.\n");
	return 0;
}
