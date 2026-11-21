/*
 * sal/wq.c - Work queue implementation
 */
#include <sal/wq.h>
#include <string.h>
#include <sys/time.h>

void wq_init(struct wq *wq)
{
	memset(wq, 0, sizeof(*wq));
}

uint64_t wq_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

int wq_register(struct wq *wq, wq_fn_t fn, void *ctx)
{
	if (wq->n_items >= WQ_MAX_ITEMS || !fn)
		return -1;

	wq->items[wq->n_items].fn = fn;
	wq->items[wq->n_items].ctx = ctx;
	wq->items[wq->n_items].deadline_ms = 0;
	wq->items[wq->n_items].periodic_ms = 0;
	return wq->n_items++;
}

void wq_schedule(struct wq *wq, int idx, int delay_ms)
{
	if (idx < 0 || idx >= wq->n_items)
		return;
	wq->items[idx].deadline_ms = wq_now_ms() + delay_ms;
	wq->items[idx].periodic_ms = 0;
}

void wq_schedule_periodic(struct wq *wq, int idx, int interval_ms)
{
	if (idx < 0 || idx >= wq->n_items || interval_ms <= 0)
		return;
	wq->items[idx].deadline_ms = wq_now_ms() + interval_ms;
	wq->items[idx].periodic_ms = interval_ms;
}

void wq_cancel(struct wq *wq, int idx)
{
	if (idx < 0 || idx >= wq->n_items)
		return;
	wq->items[idx].deadline_ms = 0;
	wq->items[idx].periodic_ms = 0;
}

void wq_pump(struct wq *wq)
{
	uint64_t now = wq_now_ms();
	int i;

	wq->now_ms = now;
	for (i = 0; i < wq->n_items; i++) {
		struct wq_work *w = &wq->items[i];

		if (w->deadline_ms == 0)
			continue;
		if (now < w->deadline_ms)
			continue;

		/* re-arm before calling (periodic may cancel itself) */
		if (w->periodic_ms > 0)
			w->deadline_ms = now + w->periodic_ms;
		else
			w->deadline_ms = 0;

		w->fn(w->ctx);
	}
}

/* === 全局单例 + initcall/pumpcall 注册 === */

#include <sal/initcall.h>

struct wq g_wq;

static int wq_sysinit(void)
{
	wq_init(&g_wq);
	return 0;
}
initcall_sal(wq_sysinit);

static void wq_syspump(void)
{
	wq_pump(&g_wq);
}
pumpcall(wq_syspump);
