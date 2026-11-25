/*
 * subsystems/net/net_core.c - Network subsystem core
 *
 * Three domains:
 *   1. Link registry: all link drivers register here
 *   2. Uplink scheduler: exclusive selection among NET_ROLE_UPLINK links
 *   3. Transport: MQTT session bound to current uplink
 *
 * Registration pattern:
 *   drv -> core: ops table (downward function calls)
 *   core -> drv: post() event delivery (upward events)
 *   app -> core: stable API calls + event callbacks
 */
#include <net/net.h>
#include <sal/evbus.h>
#include <sal/wq.h>
#include <pal/sysio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* === Internal types === */

struct net_link {
	const struct net_link_ops *ops;
	struct net_link_up up;
	struct net_link_status status;
	int enabled;
	int is_registered;
};

struct net_mqtt_cb {
	void (*peer_recv)(void *ctx, const uint8_t *d, int n);
	void *peer_recv_ctx;
};

/* === Module state (single instance) === */

static struct {
	struct net_link links[NET_LINK_MAX];
	int active_uplink;		/* NET_LINK_NONE or link_id */
	struct net_mqtt_cb mqtt_cb;
	struct evbus *evbus;
	struct wq *wq;
	int wq_timer;
	int initialized;
} net;

/* === Forward declarations === */
static void net_uplink_evaluate(void);
static void net_wq_timer(void *ctx);

/* === Link registry === */

int net_register_link(int link_id, const struct net_link_ops *ops,
		      const struct net_link_up *up)
{
	if (link_id < 0 || link_id >= NET_LINK_MAX || !ops || !up)
		return -1;
	if (net.links[link_id].is_registered)
		return -1;	/* slot occupied */

	memset(&net.links[link_id], 0, sizeof(net.links[link_id]));
	net.links[link_id].ops = ops;
	net.links[link_id].up = *up;
	net.links[link_id].status.state = NET_LINK_OFF;
	net.links[link_id].is_registered = 1;

	/* links with LAN role start enabled */
	if (ops->roles & NET_ROLE_LAN)
		net.links[link_id].enabled = 1;

	return link_id;
}

/* === Uplink scheduler === */

static int net_link_is_uplink_online(int id)
{
	struct net_link *l = &net.links[id];

	if (!l->is_registered || !l->ops)
		return 0;
	if (!(l->ops->roles & NET_ROLE_UPLINK))
		return 0;
	if (!l->enabled)
		return 0;
	return l->status.state == NET_LINK_ONLINE;
}

static void net_uplink_evaluate(void)
{
	int best = NET_LINK_NONE;
	int i;
	struct ev_uplink ev;

	/* find highest priority ONLINE uplink (lower id = higher priority) */
	for (i = 0; i < NET_LINK_MAX; i++) {
		if (net_link_is_uplink_online(i)) {
			best = i;
			break;
		}
	}

	if (best == net.active_uplink)
		return;		/* no change */

	/* disable old uplink */
	if (net.active_uplink != NET_LINK_NONE) {
		struct net_link *old = &net.links[net.active_uplink];

		if (old->ops && old->ops->enable) {
			old->ops->enable(0);
			old->enabled = 0;
		}
	}

	/* enable new uplink */
	if (best != NET_LINK_NONE) {
		struct net_link *new_l = &net.links[best];

		if (new_l->ops && new_l->ops->enable) {
			new_l->ops->enable(1);
			new_l->enabled = 1;
		}
	}

	ev.from_link = net.active_uplink;
	ev.to_link = best;
	net.active_uplink = best;

	if (net.evbus)
		evbus_publish(net.evbus, EV_NET_UPLINK_CHANGED, &ev);
}

int net_uplink_online(void)
{
	int i;

	/* ensure at least one uplink is enabled */
	for (i = 0; i < NET_LINK_MAX; i++) {
		struct net_link *l = &net.links[i];

		if (!l->is_registered || !l->ops)
			continue;
		if (!(l->ops->roles & NET_ROLE_UPLINK))
			continue;
		if (!l->enabled) {
			/* enable 失败不置 enabled — 保证下一轮
			 * 重试会再次拨号 (原版置位后永远跳过拨号) */
			if (!l->ops->enable || l->ops->enable(1) != 0)
				continue;
			l->enabled = 1;
		}
		if (l->ops->prepare_ip && l->ops->prepare_ip() == 0) {
			if (l->ops->get_status)
				l->ops->get_status(&l->status);
			if (l->status.state == NET_LINK_ONLINE)
				break;	/* this one works */
		}
	}

	net_uplink_evaluate();
	return net.active_uplink != NET_LINK_NONE ? 0 : -1;
}

int net_uplink_id(void)
{
	return net.active_uplink;
}

int net_uplink_ip(char *buf, int len)
{
	if (net.active_uplink == NET_LINK_NONE)
		return -1;
	snprintf(buf, len, "%s", net.links[net.active_uplink].status.ip);
	return 0;
}

/* === Peer domain === */

int net_peer_connect(const char *peer)
{
	int i;

	for (i = 0; i < NET_LINK_MAX; i++) {
		struct net_link *l = &net.links[i];

		if (!l->is_registered || !l->ops)
			continue;
		if (l->ops->caps & NET_CAP_PEER && l->ops->peer_connect)
			return l->ops->peer_connect(peer);
	}
	return -1;
}

int net_peer_disconnect(void)
{
	int i;

	for (i = 0; i < NET_LINK_MAX; i++) {
		struct net_link *l = &net.links[i];

		if (!l->is_registered || !l->ops)
			continue;
		if (l->ops->caps & NET_CAP_PEER && l->ops->peer_disconnect)
			return l->ops->peer_disconnect();
	}
	return -1;
}

int net_peer_send(const uint8_t *data, int len)
{
	int i;

	for (i = 0; i < NET_LINK_MAX; i++) {
		struct net_link *l = &net.links[i];

		if (!l->is_registered || !l->ops)
			continue;
		if (l->ops->caps & NET_CAP_PEER && l->ops->peer_send)
			return l->ops->peer_send(data, len);
	}
	return -1;
}

int net_peer_set_recv(void (*fn)(void *ctx, const uint8_t *d, int n), void *ctx)
{
	net.mqtt_cb.peer_recv = fn;
	net.mqtt_cb.peer_recv_ctx = ctx;
	return 0;
}

/* === Pump === */

static void net_wq_timer(void *ctx)
{
	int i;

	/* poll each registered link for status updates */
	for (i = 0; i < NET_LINK_MAX; i++) {
		struct net_link *l = &net.links[i];

		if (!l->is_registered || !l->ops || !l->ops->get_status)
			continue;
		if (!l->enabled && !(l->ops->roles & NET_ROLE_LAN))
			continue;

		l->ops->get_status(&l->status);
	}

	net_uplink_evaluate();

	/* 上电拨号 + 掉线自愈: 无在线上行时周期性拉起 (30s 节流)。
	 * 拨号含阻塞 AT (约 5-10s), 4G 不通时本就无 MQTT 连接,
	 * 阻塞无害; 30s 间隔防连续阻塞 */
	{
		static uint64_t next_bringup_ms;

		if (net.active_uplink == NET_LINK_NONE &&
		    sys_ms() >= next_bringup_ms) {
			next_bringup_ms = sys_ms() + 30000;
			printf("net: 无上行在线, 拨号拉起 4G...\n");
			net_uplink_online();
			if (net.active_uplink != NET_LINK_NONE)
				printf("net: 4G 已上线 (上行链路 %d)\n",
				       net.active_uplink);
			else
				printf("net: 4G 未上线, 30s 后重试\n");
		}
	}
}

void net_pump(void)
{
	/* MQTT polling is handled by the transport domain */
	/* Link status polling is done by the wq timer */
}

/* === Init (initcall CORE 层自动注册) + pump === */

#include <sal/initcall.h>

static int net_sysinit(void)
{
	/* 注意: 不能 memset! 静态变量加载时已清零, 且 driver 层(2)
	 * 先于 core 层(3) 注册的链路 (ec200a) 会被这里抹掉 —
	 * 曾导致注册丢失、拨号永远不触发 */
	net.active_uplink = NET_LINK_NONE;
	net.evbus = &g_evbus;
	net.wq = &g_wq;

	/* register periodic link status check (1 second) */
	net.wq_timer = wq_register(&g_wq, net_wq_timer, NULL);
	if (net.wq_timer >= 0)
		wq_schedule_periodic(&g_wq, net.wq_timer, 1000);

	return 0;
}
initcall_core(net_sysinit);

static void net_syspump(void)
{
	net_pump();
}
pumpcall(net_syspump);
