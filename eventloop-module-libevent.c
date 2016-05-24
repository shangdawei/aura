#include <stdlib.h>
#include <aura/aura.h>
#include <aura/private.h>
#include <aura/eventloop.h>
#include <aura/list.h>
#include <aura/timer.h>
#include <event.h>

struct aura_libevent_data {
	struct event_base   *ebase;
};

struct aura_libevent_node_data {
	struct event inbound_event;
	struct event outbound_event;
};

struct aura_libevent_timer {
	struct aura_timer timer;
	struct event evt;
};

static int libevent_create(struct aura_eventloop *loop)
{
    slog(3, SLOG_WARN, "evtsys-libevent: Using experimental libevent backend");
    struct aura_libevent_data *epd = calloc(1, sizeof(*epd));
    if (!epd)
		return -ENOMEM;

    loop->eventsysdata = epd;
    epd->ebase = event_base_new();

    if (!epd->ebase)
        goto err_free_epd;

    aura_eventloop_moduledata_set(loop, epd);

    return 0;

err_free_epd:
    free(epd);
    return -EIO;
}

void libevent_destroy(struct aura_eventloop *loop)
{
	struct aura_libevent_data *epd = aura_eventloop_moduledata_get(loop);
	slog(0, SLOG_DEBUG, "Destorying eventloop %x/%x", epd, epd->ebase);
	event_base_free(epd->ebase);
	free(epd);
}

static void dispatch_cb_fn(evutil_socket_t fd, short evt, void *arg)
{
	struct aura_pollfds *ap = arg;
	struct aura_node *node = ap->node;
	struct aura_eventloop *l = aura_node_eventloop_get(node);
	aura_eventloop_report_event(l, NODE_EVENT_DESCRIPTOR, ap);
}


static void libevent_fd_action(
	struct aura_eventloop *loop,
	const struct aura_pollfds *app,
	int action)
{
	struct aura_pollfds *ap = (struct aura_pollfds *) app;
	struct aura_node *node = ap->node;
	struct aura_libevent_data *epd = aura_eventloop_moduledata_get(loop);
	ap->magic = 0xdeadbeaf;

	if (ap->eventsysdata) {
		/* TODO: Error checking */
		event_del(ap->eventsysdata);
		event_free(ap->eventsysdata);
		ap->eventsysdata = NULL;
	}

	if (action == AURA_FD_ADDED) {
		ap->eventsysdata = event_new(epd->ebase, ap->fd, ap->events | EV_PERSIST,
									dispatch_cb_fn, ap);
		if (!ap->eventsysdata)
			BUG(node, "evtsys-libevent: Failed to create event");
		event_add(ap->eventsysdata, NULL);
	}
}

static void libevent_dispatch(struct aura_eventloop *loop, int flags)
{
	struct aura_libevent_data *epd = aura_eventloop_moduledata_get(loop);
	int libevent_flags = 0;
	if (flags & AURA_EVTLOOP_NONBLOCK)
		libevent_flags |= EVLOOP_NONBLOCK;
	if (flags & AURA_EVTLOOP_ONCE)
		libevent_flags |= EVLOOP_ONCE;
	if (-1 == event_base_loop(epd->ebase, libevent_flags))
		BUG(NULL, "event_base_loop() returned -1");
}

static void libevent_loopbreak(struct aura_eventloop *loop, struct timeval *tv)
{
	struct aura_libevent_data *epd = aura_eventloop_moduledata_get(loop);
	if (0 != event_base_loopexit(epd->ebase, tv))
		BUG(NULL, "event_base_loopexit() failed!");
}

static void libevent_node_added(struct aura_eventloop *loop, struct aura_node *node)
{
	struct aura_libevent_node_data *ldata = aura_node_eventloopdata_get(node);
	if (ldata)
		slog(0, SLOG_WARN, "Added node with eventsystem data present. We're likely leaking here");

	ldata = calloc(1, sizeof(*ldata));
	if (!ldata)
		BUG(node, "FATAL: Memory allocation failed!");
	aura_node_eventloopdata_set(node, ldata);
}

static void libevent_node_removed(struct aura_eventloop *loop, struct aura_node *node)
{
	struct aura_libevent_node_data *ldata = aura_node_eventloopdata_get(node);
	if (!ldata) {
		slog(0, SLOG_ERROR, "Attempt to remove node with no eventloopdata. WTF?");
		return;
	}
	event_del(&ldata->inbound_event);
	event_del(&ldata->outbound_event);
	aura_node_eventloopdata_set(node, NULL);
	free(ldata);
}

static void libevent_timer_create(struct aura_eventloop *loop, struct aura_timer *tm)
{
	/* Nothing to do */
}

static void timer_dispatch_fn(int fd, short events, void *arg)
{
	struct aura_timer *tm = arg;
	tm->callback(tm->node, tm, tm->callback_arg);
	if (!(tm->flags & AURA_TIMER_PERIODIC))
		tm->is_active = false;
	else if (tm->flags & AURA_TIMER_FREE)
		aura_timer_destroy(tm);
}

static void libevent_timer_start(struct aura_eventloop *loop, struct aura_timer *tm)
{
	struct aura_libevent_timer *ltm;
	struct aura_libevent_data *epd = aura_eventloop_moduledata_get(loop);
	ltm = container_of(tm, struct aura_libevent_timer, timer);
	int flags = 0;
	if (tm->flags & AURA_TIMER_PERIODIC)
		flags |= EV_PERSIST;
	event_assign(&ltm->evt, epd->ebase, -1, flags, timer_dispatch_fn, tm);
	event_add(&ltm->evt, &tm->tv);
}

static void libevent_timer_stop(struct aura_eventloop *loop, struct aura_timer *tm)
{
	struct aura_libevent_timer *ltm;
	ltm = container_of(tm, struct aura_libevent_timer, timer);
	event_del(&ltm->evt);
}

static void libevent_timer_destroy(struct aura_eventloop *loop, struct aura_timer *tm)
{
	/* Nothing to do */
}


static struct aura_eventloop_module levt =
{
    .name = "libevent",
	.timer_size = sizeof(struct aura_libevent_timer),
	.timer_create = libevent_timer_create,
	.timer_start = libevent_timer_start,
	.timer_stop = libevent_timer_stop,
	.timer_destroy = libevent_timer_destroy,
    .create = libevent_create,
    .destroy  = libevent_destroy,
	.fd_action = libevent_fd_action,
	.dispatch = libevent_dispatch,
	.loopbreak = libevent_loopbreak,
	.node_added = libevent_node_added,
	.node_removed = libevent_node_removed,
};

AURA_EVENTLOOP_MODULE(levt);

#if 0

static void interrupt_cb_fn(evutil_socket_t fd, short evt, void *arg)
{
	struct aura_libevent_data *epd = arg;
	aura_eventloop_report_event(epd->loopdata, NULL);
	event_base_loopbreak(epd->ebase);
}




#define NUM_EVTS 1
int aura_eventsys_backend_wait(void *backend, int timeout_ms)
{
	struct aura_libevent_data *epd = backend;

	struct timeval delay;
	delay.tv_usec = timeout_ms*1000;
	delay.tv_sec = 0;

	if (0 != event_add(epd->ievt, &delay))
		return BUG(NULL, "evtsys-libevent: Failed to add event");


	int ret = event_base_dispatch(epd->ebase);
	if (-1 == ret)
		return BUG(NULL, "event_base_dispatch returned -1");

	return ret;
}


struct event_base *aura_eventsys_backend_get_ebase(void *backend)
{
	struct aura_libevent_data *epd = backend;
	return epd->ebase;
}

#endif
