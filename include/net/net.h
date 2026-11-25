/*
 * include/net/net.h - Network subsystem public API
 *
 * This is the ONLY header other subsystems should include for net.
 * Internal structure and driver implementations are private.
 *
 * Link roles:
 *   UPLINK  - candidate for cloud connectivity (exclusive selection)
 *   LAN     - local service network (always on)
 *   PEER    - proximity connection (SLE, non-IP)
 *
 * Downward = function calls (app -> core -> drv ops)
 * Upward   = events (drv -> core -> registered callbacks)
 */
#ifndef SUBSYS_NET_H
#define SUBSYS_NET_H

#include <stdint.h>

/* Forward declaration for SAL types used in net_init_with */
struct evbus;
struct wq;

/* Link capabilities */
#define NET_CAP_IP	0x01	/* can carry IP/MQTT traffic */
#define NET_CAP_PEER	0x02	/* peer-to-peer connections */
#define NET_CAP_SCAN	0x04	/* can scan for peers/networks */

/* Link roles */
#define NET_ROLE_UPLINK	0x10	/* candidate for cloud uplink */
#define NET_ROLE_LAN	0x20	/* local service network */
#define NET_ROLE_PEER	0x40	/* peer connections */

/* Link states */
enum net_link_state {
	NET_LINK_OFF = 0,
	NET_LINK_STANDBY,
	NET_LINK_READY,		/* physical/association OK */
	NET_LINK_ONLINE,	/* has IP address */
};

/* Link IDs (stable, used for logging and events) */
#define NET_LINK_NONE		-1
#define NET_LINK_EC200A		0	/* 4G modem (default uplink) */
#define NET_LINK_ETH_WAN	1	/* eth0.2 (wired uplink) */
#define NET_LINK_WLAN		2	/* wlan0 (AP mode, LAN role) */
#define NET_LINK_WS63_SLE	3	/* WS63 SparkLink (peer role) */
#define NET_LINK_MAX		4

struct net_link_status {
	int state;
	char ip[48];
	int rssi;		/* signal strength, or 0 if N/A */
	int peers;		/* connected peer count */
};

/* Driver ops: implemented by each link driver, registered to core */
struct net_link_ops {
	uint32_t caps;
	uint32_t roles;
	const char *name;

	int  (*enable)(int on);
	int  (*get_status)(struct net_link_status *st);

	/* NET_CAP_IP links: */
	int  (*prepare_ip)(void);

	/* NET_CAP_PEER links: */
	int  (*peer_adv)(int on);
	int  (*peer_scan)(int on);
	int  (*peer_connect)(const char *peer);
	int  (*peer_disconnect)(void);
	int  (*peer_send)(const uint8_t *data, int len);
};

/* Upward event delivery (core provides this to each driver) */
struct net_link_up {
	void (*post)(void *handle, int link_id, int ev, const void *data, int len);
	void *handle;
};

/* Link events (delivered via net_link_up.post) */
enum net_link_ev {
	NET_EV_STATE_CHANGED = 1,	/* data: struct net_link_status */
	NET_EV_PEER_CONNECTED,		/* data: const char *peer_name */
	NET_EV_PEER_DISCONNECTED,
	NET_EV_PEER_DATA,		/* data: raw bytes */
	NET_EV_SCAN_RESULT,		/* data: scan entry */
};

/* === Subsystem core API (called by app/orchestrator) === */

/* Register a link driver.  Returns link_id or -1 on error. */
int net_register_link(int link_id, const struct net_link_ops *ops,
		      const struct net_link_up *up);

/* Uplink scheduling: ensure the best available IP link is ONLINE.
 * Returns 0 on success, -1 if no uplink available. */
int net_uplink_online(void);

/* Get current uplink info */
int net_uplink_id(void);
int net_uplink_ip(char *buf, int len);

/* Peer domain (SLE): connect/send/receive */
int net_peer_connect(const char *peer);
int net_peer_disconnect(void);
int net_peer_send(const uint8_t *data, int len);
int net_peer_set_recv(void (*fn)(void *ctx, const uint8_t *d, int n), void *ctx);

/* Transport domain: MQTT */
int net_mqtt_connect(const char *host, int port, const char *client_id);
int net_mqtt_publish(const char *topic, const uint8_t *data, int len);
int net_mqtt_subscribe(const char *topic);
int net_mqtt_poll(int timeout_ms);	/* returns 1=msg, 0=timeout, -1=err */
int net_mqtt_connected(void);
void net_mqtt_disconnect(void);

/* Pump: called from main loop */
void net_pump(void);

/* Init 经 initcall_core(net_sysinit) 自动注册, 无需显式调用 */

#endif /* SUBSYS_NET_H */
