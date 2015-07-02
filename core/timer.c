#include <timer.h>
#include <timebase.h>
#include <lock.h>
#include <fsp.h>
#include <device.h>
#include <opal.h>

#ifdef __TEST__
#define this_cpu()	((void *)-1)
#define cpu_relax()
#else
#include <cpu.h>
#endif

//#define DBG(fmt...) prlog(PR_INFO, "TIMER:" fmt)
#define DBG(fmt...) do { } while(0)

/* Heartbeat requested from Linux */
#define HEARTBEAT_DEFAULT_MS	2000

static struct lock timer_lock = LOCK_UNLOCKED;
static LIST_HEAD(timer_list);
static LIST_HEAD(timer_poll_list);
static bool timer_in_poll;
static uint64_t timer_poll_gen;

void init_timer(struct timer *t, timer_func_t expiry, void *data)
{
	t->link.next = t->link.prev = NULL;
	t->target = 0;
	t->expiry = expiry;
	t->user_data = data;
	t->running = NULL;
}

static void __remove_timer(struct timer *t)
{
	list_del(&t->link);
	t->link.next = t->link.prev = NULL;
}

static void __sync_timer(struct timer *t)
{
	sync();

	/* Guard against re-entrancy */
	assert(t->running != this_cpu());

	while (t->running) {
		unlock(&timer_lock);
		cpu_relax();
		/* Should we call the pollers here ? */
		lock(&timer_lock);
	}
}

void sync_timer(struct timer *t)
{
	lock(&timer_lock);
	__sync_timer(t);
	unlock(&timer_lock);
}

void cancel_timer(struct timer *t)
{
	lock(&timer_lock);
	__sync_timer(t);
	if (t->link.next)
		__remove_timer(t);
	unlock(&timer_lock);
}

void cancel_timer_async(struct timer *t)
{
	lock(&timer_lock);
	if (t->link.next)
		__remove_timer(t);
	unlock(&timer_lock);
}

static void __schedule_timer_at(struct timer *t, uint64_t when)
{
	struct timer *lt;

	DBG("Scheduling timer %p at %llx\n", t, when);

	if (t->link.next) {
		DBG("  - removing first\n");
		__remove_timer(t);
	}
	t->target = when;
	if (when == TIMER_POLL) {
		t->gen = timer_poll_gen;
		list_add_tail(&timer_poll_list, &t->link);
		DBG("  - added to poll list\n");
	} else {
		DBG("  - scanning timer list...\n");
		list_for_each(&timer_list, lt, link) {
			DBG("     found %p at %llx\n", lt, lt->target);
			if (when < lt->target) {
				DBG("     ! adding before !\n");
				list_add_before(&timer_list, &t->link,
						&lt->link);
				goto bail;
			}
		}
		list_add_tail(&timer_list, &t->link);
		DBG("     ! added to tail !\n");
	}
 bail:
	lt = list_top(&timer_list, struct timer, link);
	if (lt) {
		DBG("  - updating slw to %llx\n", lt->target);
		slw_update_timer_expiry(lt->target);
	}
}

void schedule_timer_at(struct timer *t, uint64_t when)
{
	lock(&timer_lock);
	__schedule_timer_at(t, when);
	unlock(&timer_lock);
}

uint64_t schedule_timer(struct timer *t, uint64_t how_long)
{
	uint64_t now = mftb();

	if (how_long == TIMER_POLL)
		schedule_timer_at(t, TIMER_POLL);
	else
		schedule_timer_at(t, now + how_long);

	return now;
}

static void __check_poll_timers(uint64_t now)
{
	struct timer *t;

	/* Don't call this from multiple CPUs at once */
	if (timer_in_poll)
		return;
	timer_in_poll = true;

	/*
	 * Poll timers might re-enqueue themselves and don't have an
	 * expiry so we can't do like normal timers and just run until
	 * we hit a wall. Instead, each timer has a generation count,
	 * which we set to the current global gen count when we schedule
	 * it and update when we run it. It will only be considered if
	 * the generation count is different than the current one. We
	 * don't try to compare generations being larger or smaller
	 * because at boot, this can be called quite quickly and I want
	 * to be safe vs. wraps.
	 */
	timer_poll_gen++;
	for (;;) {
		t = list_top(&timer_poll_list, struct timer, link);

		/* Top timer has a different generation than current ? Must
		 * be older
		 */
		if (!t || t->gen == timer_poll_gen)
			break;

		/* Top of list still running, we have to delay handling
		 * it. For now just skip until the next poll, when we have
		 * SLW interrupts, we'll probably want to trip another one
		 * ASAP
		 */
		if (t->running)
			break;

		/* Allright, first remove it and mark it running */
		__remove_timer(t);
		t->running = this_cpu();

		/* Now we can unlock and call it's expiry */
		unlock(&timer_lock);
		t->expiry(t, t->user_data, now);

		/* Re-lock and mark not running */
		lock(&timer_lock);
		t->running = NULL;
	}
	timer_in_poll = false;
}

static void __check_timers(uint64_t now)
{
	struct timer *t;

	for (;;) {
		t = list_top(&timer_list, struct timer, link);

		/* Top of list not expired ? that's it ... */
		if (!t || t->target > now)
			break;

		DBG("Timer %p expired at %llx (now=%llx)\n", t, t->target, now);

		/* Top of list still running, we have to delay handling
		 * it. For now just skip until the next poll, when we have
		 * SLW interrupts, we'll probably want to trip another one
		 * ASAP
		 */
		if (t->running) {
			DBG("     ! already running !\n");
			break;
		}

		/* Allright, first remove it and mark it running */
		__remove_timer(t);
		t->running = this_cpu();

		/* Now we can unlock and call it's expiry */
		unlock(&timer_lock);
		t->expiry(t, t->user_data, now);

		/* Re-lock and mark not running */
		lock(&timer_lock);
		t->running = NULL;

		/* Update time stamp */
		now = mftb();
	}
}

void check_timers(bool from_interrupt)
{
	struct timer *t;
	uint64_t now = mftb();

	/* This is the polling variant, the SLW interrupt path, when it
	 * exists, will use a slight variant of this that doesn't call
	 * the pollers
	 */

	/* Lockless "peek", a bit racy but shouldn't be a problem */
	t = list_top(&timer_list, struct timer, link);
	if (list_empty(&timer_poll_list) && (!t || t->target > now))
		return;

	/* Take lock and try again */
	lock(&timer_lock);
	if (!from_interrupt)
		__check_poll_timers(now);
	__check_timers(now);
	unlock(&timer_lock);
}

#ifndef __TEST__

void late_init_timers(void)
{
	/* Add a property requesting the OS to call opal_poll_event() at
	 * a specified interval in order for us to run our background
	 * low priority poller if we don't have an SLW timer facility.
	 *
	 * We use a value in milliseconds, we don't want this to ever be
	 * faster than that.
	 *
	 * If the SLW timer is available, we instead use a timer to do
	 * the 2s poll.
	 */
	if (slw_timer_ok() || fsp_present()) {
		dt_add_property_cells(opal_node, "ibm,heartbeat-ms",
				      HEARTBEAT_DEFAULT_MS);
	} else {
		dt_add_property_cells(opal_node, "ibm,heartbeat-ms",
				      HEARTBEAT_DEFAULT_MS / 10);
	}
}
#endif
