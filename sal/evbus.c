/*
 * sal/evbus.c - Event bus implementation
 */
#include <sal/evbus.h>
#include <string.h>

void evbus_init(struct evbus *bus)
{
	memset(bus, 0, sizeof(*bus));
}

int evbus_subscribe(struct evbus *bus, enum ev_id id,
		    ev_handler_t fn, void *ctx)
{
	if (bus->n_subs >= EVBUS_MAX_SUBS || !fn)
		return -1;

	bus->subs[bus->n_subs].id = id;
	bus->subs[bus->n_subs].fn = fn;
	bus->subs[bus->n_subs].ctx = ctx;
	bus->n_subs++;
	return 0;
}

void evbus_publish(struct evbus *bus, enum ev_id id, const void *data)
{
	int i;

	for (i = 0; i < bus->n_subs; i++) {
		if (bus->subs[i].id == id)
			bus->subs[i].fn(bus->subs[i].ctx, id, data);
	}
}

/* === 全局单例 + initcall 注册 === */

#include <sal/initcall.h>

struct evbus g_evbus;

static int evbus_sysinit(void)
{
	evbus_init(&g_evbus);
	return 0;
}
initcall_sal(evbus_sysinit);
