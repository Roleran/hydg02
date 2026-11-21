/*
 * sal/wq.h - Work queue with timers
 *
 * Deferred work + periodic timers for the single-threaded main loop.
 * Formalizes the "pump" pattern: each subsystem registers work items
 * and timers; the main loop processes them in order.
 *
 * Design rules:
 *   - Fixed slots (no dynamic allocation)
 *   - Work items run in main loop context
 *   - Timers are ms-resolution, checked on each pump cycle
 */
#ifndef SAL_WQ_H
#define SAL_WQ_H

#include <stdint.h>

typedef void (*wq_fn_t)(void *ctx);

struct wq_work {
	wq_fn_t fn;
	void *ctx;
	uint64_t deadline_ms;	/* 0 = not scheduled */
	int periodic_ms;	/* 0 = one-shot */
};

#define WQ_MAX_ITEMS 64

struct wq {
	struct wq_work items[WQ_MAX_ITEMS];
	int n_items;
	uint64_t now_ms;
};

/* 全局单例 (initcall_sal 注册初始化, 见 sal/wq.c) */
extern struct wq g_wq;

void wq_init(struct wq *wq);
uint64_t wq_now_ms(void);

/* Register a work item (one slot per subsystem/function).  Returns
 * index or -1 if full. */
int wq_register(struct wq *wq, wq_fn_t fn, void *ctx);

/* Schedule a registered item.  Periodic items re-arm automatically. */
void wq_schedule(struct wq *wq, int idx, int delay_ms);
void wq_schedule_periodic(struct wq *wq, int idx, int interval_ms);
void wq_cancel(struct wq *wq, int idx);

/* Process all due items.  Call from main loop. */
void wq_pump(struct wq *wq);

#endif /* SAL_WQ_H */
