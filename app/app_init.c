/*
 * app/app_init.c - 应用编排层初始化 (initcall APP 层)
 *
 * 事件订阅、业务模块装配在此注册。main.c 不再出现任何模块细节。
 */
#include <sal/initcall.h>
#include <sal/evbus.h>
#include <net/net.h>

#include <stdio.h>

/* === 演示: 链路事件订阅 (后续 online/proto 接管) === */

static void on_link_change(void *ctx, enum ev_id id, const void *data)
{
	const struct ev_uplink *ev = data;

	(void)ctx;
	printf("[event] uplink changed: %d -> %d\n",
	       ev ? ev->from_link : -1, ev ? ev->to_link : -1);
}

static void on_link_up(void *ctx, enum ev_id id, const void *data)
{
	(void)ctx;
	printf("[event] link %d up\n", data ? *(const int *)data : -1);
}

static int app_sysinit(void)
{
	evbus_subscribe(&g_evbus, EV_NET_UPLINK_CHANGED, on_link_change, NULL);
	evbus_subscribe(&g_evbus, EV_NET_LINK_UP, on_link_up, NULL);
	return 0;
}
initcall_app(app_sysinit);
