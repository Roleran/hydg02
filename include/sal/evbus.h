/*
 * sal/evbus.h - System event bus (pub/sub)
 *
 * Cross-subsystem decoupling mechanism.  Subsystems publish state
 * changes; any subsystem (or the orchestrator) subscribes.  Handlers
 * run in the main pump context (single-threaded, no locking).
 *
 * Design rules:
 *   - Fixed subscriber slots (no dynamic allocation)
 *   - Handlers must be non-blocking (they run in the main loop)
 *   - Events are fire-and-forget (no return value, no queuing)
 */
#ifndef SAL_EVBUS_H
#define SAL_EVBUS_H

/* System event IDs (cross-subsystem, add as needed) */
enum ev_id {
	EV_NONE = 0,

	/* Network events */
	EV_NET_LINK_UP,		/* data: link_id */
	EV_NET_LINK_DOWN,	/* data: link_id */
	EV_NET_UPLINK_CHANGED,	/* data: struct ev_uplink */

	/* OTA events */
	EV_OTA_STARTED,
	EV_OTA_DONE,		/* data: int result */
	EV_OTA_PROGRESS,	/* data: int percent */

	/* Power events */
	EV_POWER_LOW,		/* data: int percent */
	EV_POWER_CRITICAL,
	EV_POWER_NORMAL,

	/* Monitor events */
	EV_MON_STALE,		/* data: int subsystem_id */
	EV_MON_REBOOT_CAUSE,	/* data: int cause */

	/* Time events */
	EV_TIME_SYNCED,		/* data: int source */

	EV_ID_MAX
};

struct ev_uplink {
	int from_link;		/* old link id, -1 = none */
	int to_link;		/* new link id */
};

typedef void (*ev_handler_t)(void *ctx, enum ev_id id, const void *data);

struct ev_sub {
	enum ev_id id;
	ev_handler_t fn;
	void *ctx;
};

#define EVBUS_MAX_SUBS 32

struct evbus {
	struct ev_sub subs[EVBUS_MAX_SUBS];
	int n_subs;
};

/* 全局单例 (initcall_sal 注册初始化, 见 sal/evbus.c) */
extern struct evbus g_evbus;

void evbus_init(struct evbus *bus);
int evbus_subscribe(struct evbus *bus, enum ev_id id,
		    ev_handler_t fn, void *ctx);
void evbus_publish(struct evbus *bus, enum ev_id id, const void *data);

#endif /* SAL_EVBUS_H */
