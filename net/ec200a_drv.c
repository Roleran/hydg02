/*
 * subsystems/net/ec200a_drv.c - EC200A 4G modem driver
 *
 * Migrated from src/modem.c + src/at_port.c into the net subsystem
 * driver pattern.  Implements net_link_ops for the 4G uplink.
 *
 * Data plane: usb0 (ECM, cdc_ether), configured via netifd (wwan).
 * Control plane: AT commands on /dev/ttyUSB2.
 */
#include <net/net.h>
#include <sal/initcall.h>
#include <pal/sysio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif

#define EC200A_AT_DEV		"/dev/ttyUSB2"
#define EC200A_AT_BAUD		115200
#define EC200A_NET_IF		"usb0"
#define EC200A_AT_TIMEOUT_MS	3000

struct ec200a {
	int fd;
	int online;
	char imei[32];
	char iccid[32];
};

static struct ec200a ec200a;

/* === AT port helpers (from src/at_port.c, simplified) === */

static int at_open(void)
{
	struct termios tio;

	if (ec200a.fd >= 0)
		return 0;

	ec200a.fd = open(EC200A_AT_DEV, O_RDWR | O_NOCTTY);
	if (ec200a.fd < 0) {
		printf("ec200a: at_open 失败 %s: %s\n",
		       EC200A_AT_DEV, strerror(errno));
		return -1;
	}

	tcgetattr(ec200a.fd, &tio);
	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tcsetattr(ec200a.fd, TCSANOW, &tio);
	return 0;
}

__attribute__((unused))
static void at_close(void)
{
	if (ec200a.fd >= 0) {
		close(ec200a.fd);
		ec200a.fd = -1;
	}
}

static int at_cmd(const char *cmd, char *resp, int resp_cap, int timeout_ms)
{
	char buf[256];
	int n, total = 0;
	struct pollfd pfd;

	if (ec200a.fd < 0 && at_open() != 0)
		return -1;

	/* flush */
	tcflush(ec200a.fd, TCIOFLUSH);

	/* send */
	n = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
	if (write(ec200a.fd, buf, n) != n)
		return -1;

	/* receive until OK/ERROR or timeout */
	pfd.fd = ec200a.fd;
	pfd.events = POLLIN;
	while (total < resp_cap - 1) {
		int ret = poll(&pfd, 1, timeout_ms);

		if (ret <= 0)
			break;
		n = read(ec200a.fd, resp + total, resp_cap - 1 - total);
		if (n <= 0)
			break;
		total += n;
		resp[total] = '\0';
		if (strstr(resp, "OK"))
			return 0;
		if (strstr(resp, "ERROR"))
			return -1;
	}
	resp[total] = '\0';
	return total > 0 ? 0 : -1;
}

/* === Driver ops implementation === */

static int ec200a_enable(int on)
{
	char resp[512];

	if (on) {
		if (at_open() != 0)
			return -1;

		/* check SIM */
		if (at_cmd("AT+CPIN?", resp, sizeof(resp),
			   EC200A_AT_TIMEOUT_MS) != 0) {
			printf("ec200a: CPIN? 无响应\n");
			return -1;
		}
		if (!strstr(resp, "READY")) {
			printf("ec200a: SIM 未就绪: %s\n", resp);
			return -1;
		}

		/* dial: QNETDEVCTL=3,1 = 自动连接 + 写入模组 NV
		 * (首次成功后模组开机注网自拨, 此处为幂等保底) */
		printf("ec200a: 4G 拨号中 (QNETDEVCTL=3,1)...\n");
		if (at_cmd("AT+QNETDEVCTL=3,1", resp, sizeof(resp),
			   EC200A_AT_TIMEOUT_MS) == 0)
			printf("ec200a: 拨号指令 OK\n");
		else
			printf("ec200a: 拨号指令无响应 (模组可能自拨中)\n");
		/* fall back to CGACT if QNETDEVCTL not supported */
		at_cmd("AT+CGACT=1,1", resp, sizeof(resp),
		       EC200A_AT_TIMEOUT_MS);

		ec200a.online = 1;
		return 0;
	}

	/* disable: hang up */
	at_cmd("AT+QNETDEVCTL=0,1,0", resp, sizeof(resp),
	       EC200A_AT_TIMEOUT_MS);
	ec200a.online = 0;
	return 0;
}

static int ec200a_get_status(struct net_link_status *st)
{
	char resp[512];
	FILE *fp;
	char line[256];

	memset(st, 0, sizeof(*st));

	if (ec200a.fd < 0 || !ec200a.online) {
		st->state = NET_LINK_OFF;
		return 0;
	}

	/* check default route via usb0 */
	fp = fopen("/proc/net/route", "r");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			char iface[16];
			unsigned long dest = 1;

			if (sscanf(line, "%15s %lx", iface, &dest) >= 2) {
				if (strcmp(iface, EC200A_NET_IF) == 0 &&
				    dest == 0) {
					st->state = NET_LINK_ONLINE;
					break;
				}
			}
		}
		fclose(fp);
	}

	if (st->state != NET_LINK_ONLINE)
		st->state = NET_LINK_READY;

	/* get signal quality — 30s 节流:
	 * wq 每秒轮询本函数, AT+CSQ 带 2s 超时, 不节流会
	 * 周期性阻塞主循环 (MQTT 会断, 参见 90s 卡死教训) */
	{
		static uint64_t last_csq_ms;

		if (sys_ms() - last_csq_ms > 30000) {
			last_csq_ms = sys_ms();
			if (at_cmd("AT+CSQ", resp, sizeof(resp), 2000) == 0) {
				int rssi = 0;

				if (sscanf(resp, "+CSQ: %d", &rssi) == 1)
					st->rssi = rssi;
			}
		}
	}

	/* get IP address */
	if (st->state == NET_LINK_ONLINE) {
		int fd = socket(AF_INET, SOCK_DGRAM, 0);

		if (fd >= 0) {
			struct ifreq ifr;

			memset(&ifr, 0, sizeof(ifr));
			strncpy(ifr.ifr_name, EC200A_NET_IF,
				IF_NAMESIZE - 1);
			if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
				struct sockaddr_in *sin =
					(struct sockaddr_in *)&ifr.ifr_addr;

				inet_ntop(AF_INET, &sin->sin_addr,
					  st->ip, sizeof(st->ip));
			}
			close(fd);
		}
	}

	return 0;
}

static int ec200a_prepare_ip(void)
{
	char resp[256];

	/* ensure PDP is active */
	at_cmd("AT+CGACT=1,1", resp, sizeof(resp), EC200A_AT_TIMEOUT_MS);

	/* netifd handles the actual IP config (wwan interface,
	 * static 192.168.43.100/24 with gateway 192.168.43.1).
	 * Trigger ifup in case it hasn't run yet. */
	system("ifup wwan 2>/dev/null");
	usleep(500000);		/* wait for interface */

	return 0;
}

/* === Driver registration === */

static const struct net_link_ops ec200a_ops = {
	.caps		= NET_CAP_IP,
	.roles		= NET_ROLE_UPLINK,
	.name		= "ec200a-4g",
	.enable		= ec200a_enable,
	.get_status	= ec200a_get_status,
	.prepare_ip	= ec200a_prepare_ip,
};

static struct net_link_up ec200a_up;

static void ec200a_post(void *handle, int link_id, int ev,
			const void *data, int len)
{
	(void)handle;
	(void)link_id;
	(void)ev;
	(void)data;
	(void)len;
	/* In the full implementation, this would forward events
	 * to the net core for dispatch to subscribers.  For P1
	 * skeleton, this is a placeholder. */
}

static int ec200a_drv_init(void)
{
	memset(&ec200a, 0, sizeof(ec200a));
	ec200a.fd = -1;

	ec200a_up.post = ec200a_post;
	ec200a_up.handle = NULL;

	return net_register_link(NET_LINK_EC200A, &ec200a_ops, &ec200a_up);
}
initcall_driver(ec200a_drv_init);

/* Probe: is the 4G modem physically present and responding?
 * Returns 1 if device node exists and answers "AT", 0 otherwise.
 * Registration still succeeds without the device (hotplug support),
 * the link simply stays in NET_LINK_OFF state. */
int ec200a_dev_present(void)
{
	char resp[64];

	if (access(EC200A_AT_DEV, F_OK) != 0)
		return 0;

	if (at_open() != 0)
		return 0;

	if (at_cmd("AT", resp, sizeof(resp), 1000) != 0)
		return 0;

	return 1;
}

/* Read IMEI/ICCID (needed by the orchestrator for registration) */
int ec200a_get_ids(char *imei, int imei_len, char *iccid, int iccid_len)
{
	char resp[256];
	char *p;

	if (at_open() != 0)
		return -1;

	/* IMEI */
	if (at_cmd("AT+CGSN", resp, sizeof(resp), 2000) == 0) {
		p = resp;
		while (*p && (*p < '0' || *p > '9'))
			p++;
		if (imei)
			snprintf(imei, imei_len, "%s", p);
		/* strip trailing \r\n */
		p = imei;
		if (imei) {
			while (*p && *p >= '0' && *p <= '9')
				p++;
			*p = '\0';
		}
	}

	/* ICCID */
	if (at_cmd("AT+QCCID", resp, sizeof(resp), 2000) == 0) {
		p = strstr(resp, "+QCCID: ");
		if (p) {
			p += 8;
			if (iccid)
				snprintf(iccid, iccid_len, "%s", p);
			p = iccid;
			if (iccid) {
				while (*p && (*p == '0' ||
				       (*p >= '0' && *p <= '9')))
					p++;
				*p = '\0';
			}
		}
	}

	return 0;
}
