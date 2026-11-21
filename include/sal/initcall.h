/*
 * include/sal/initcall.h - Linux 内核风格 initcall (分层初始化注册)
 *
 * 各模块用宏把 init 函数注册进按层级命名的链接段
 * (.initcall0.init ~ .initcall5.init), initcall.lds 按层序拼接,
 * initcalls_run() 逐层调用。main.c 只需一次 initcalls_run()。
 *
 * 层级 (小者先执行; 同层按链接顺序, 不得有跨模块依赖):
 *   EARLY  0  基础设施: 配置载入, 其他模块依赖其结果
 *   SAL    1  系统抽象层: evbus / wq
 *   DRIVER 2  设备驱动: ec200a / ws63 / es8311 ...
 *   CORE   3  子系统核心: net / ota / loc ...
 *   APP    4  应用编排: online / proto / 业务模块
 *   LATE   5  收尾: 自检 / 监视狗
 *
 * 同级模块之间不得存在 init 顺序依赖; 需要先后关系时用不同层级表达。
 *
 * 实现说明 (与内核一致): 每层一个段, 段内是 {fn, name} 定长条目数组,
 * 条目大小为指针对齐的整数倍, 跨目标文件无填充, 迭代安全。
 * 段边界符号由 initcall.lds (INSERT AFTER 增补式链接脚本) 定义。
 *
 * pumpcall: 主循环每迭代调用的钩子, 单层无优先级, 注册进 .pumpcalls,
 * pumpcalls_run() 逐个调用。钩子必须非阻塞、可重入。
 */
#ifndef SAL_INITCALL_H
#define SAL_INITCALL_H

struct initcall_entry {
	int (*fn)(void);
	const char *name;
};

#define INIT_LEVEL_EARLY	0
#define INIT_LEVEL_SAL		1
#define INIT_LEVEL_DRIVER	2
#define INIT_LEVEL_CORE		3
#define INIT_LEVEL_APP		4
#define INIT_LEVEL_LATE		5
#define INIT_LEVEL_COUNT	6

#define __initcall_register(fn, lvl) \
	static const struct initcall_entry __ic_##fn \
	__attribute__((used, section(".initcall" #lvl ".init"))) = { (fn), #fn }

#define initcall_early(fn)	__initcall_register(fn, 0)
#define initcall_sal(fn)	__initcall_register(fn, 1)
#define initcall_driver(fn)	__initcall_register(fn, 2)
#define initcall_core(fn)	__initcall_register(fn, 3)
#define initcall_app(fn)	__initcall_register(fn, 4)
#define initcall_late(fn)	__initcall_register(fn, 5)

/* 逐层执行全部 initcall (每层只执行一次)。返回失败个数。 */
int initcalls_run(void);

/* 仅执行指定层级。返回失败个数, 层级非法返回 -1。 */
int initcalls_run_level(int level);

/* 已注册的 initcall 总数 (自测用) */
int initcall_count(void);

/* 打印注册清单 (层级+名字, 调试用) */
void initcalls_dump(void);

/* === pumpcall === */

struct pumpcall_entry {
	void (*fn)(void);
	const char *name;
};

#define pumpcall(fn) \
	static const struct pumpcall_entry __pc_##fn \
	__attribute__((used, section(".pumpcalls"))) = { (fn), #fn }

/* 主循环每迭代调用全部 pumpcall */
void pumpcalls_run(void);

int pumpcall_count(void);

#endif /* SAL_INITCALL_H */
