#include <string.h>

#include "discord.h"
#include "discord-internal.h"

#define DISCORD_TIMER_ALLOWED_FLAGS                                           \
    (DISCORD_TIMER_MILLISECONDS | DISCORD_TIMER_MICROSECONDS                  \
     | DISCORD_TIMER_DELETE | DISCORD_TIMER_DELETE_AUTO                       \
     | DISCORD_TIMER_INTERVAL_FIXED)

static int
cmp_timers(const void *a, const void *b)
{
    const int64_t l = *(int64_t *)a;
    const int64_t r = *(int64_t *)b;
    if (l == r || (l < 0 && r < 0)) return 0;
    if (l < 0) return 1;
    if (r < 0) return -1;
    return l > r ? 1 : -1;
}

void
discord_timers_init(struct discord_timers *timers, struct io_poller *io)
{
    timers->q = priority_queue_create(
        sizeof(int64_t), sizeof(struct discord_timer), cmp_timers, 0);
    timers->io = io;
    pthread_mutex_init(&timers->lock, NULL);
    pthread_cond_init(&timers->cond, NULL);
}

static void
discord_timers_cancel_all(struct discord *client,
                          struct discord_timers *timers)
{
    struct discord_timer timer;
    while ((timer.id = priority_queue_pop(timers->q, NULL, &timer))) {
        timer.flags |= DISCORD_TIMER_CANCELED;
        if (timer.cb) timer.cb(client, &timer);
    }
}

void
discord_timers_cleanup(struct discord *client, struct discord_timers *timers)
{
    pthread_cond_destroy(&timers->cond);
    pthread_mutex_destroy(&timers->lock);
    priority_queue_set_max_capacity(timers->q, 0);
    discord_timers_cancel_all(client, timers);
    priority_queue_destroy(timers->q);
}

int64_t
discord_timers_get_next_trigger(struct discord_timers *const timers[],
                                size_t n,
                                int64_t now,
                                int64_t max_time)
{
    if (max_time == 0) return 0;

    for (unsigned i = 0; i < n; i++) {
        int64_t trigger;
        if (0 != pthread_mutex_trylock(&timers[i]->lock)) return 0;

        if (priority_queue_peek(timers[i]->q, &trigger, NULL)) {
            if (trigger < 0) continue;

            if (trigger <= now)
                max_time = 0;
            else if (max_time > trigger - now)
                max_time = trigger - now;
        }
        pthread_mutex_unlock(&timers[i]->lock);
    }
    return max_time;
}

static unsigned
_discord_timer_ctl_no_lock(struct discord *client,
                           struct discord_timers *timers,
                           struct discord_timer *timer_ret)
{
    struct discord_timer timer;
    memcpy(&timer, timer_ret, sizeof timer);

    int64_t key = -1;
    if (timer.id) {
        if (!priority_queue_get(timers->q, timer.id, &key, NULL)) return 0;

        if (timer.flags & DISCORD_TIMER_GET) {
            timer_ret->id =
                priority_queue_get(timers->q, timer.id, NULL, timer_ret);
            if (timer.flags == DISCORD_TIMER_GET) return timer_ret->id;
        }
    }

    int64_t now = -1;
    if (timer.delay >= 0) {
        now = (int64_t)discord_timestamp_us(client)
              + ((timer.flags & DISCORD_TIMER_MICROSECONDS)
                     ? timer.delay
                     : timer.delay * 1000);
    }
    if (timer.flags & (DISCORD_TIMER_DELETE | DISCORD_TIMER_CANCELED)) now = 0;

    timer.flags &= (DISCORD_TIMER_ALLOWED_FLAGS | DISCORD_TIMER_CANCELED);

    if (!timer.id) {
        return priority_queue_push(timers->q, &now, &timer);
    }
    else {
        if (timers->active.timer && timers->active.timer->id == timer.id)
            timers->active.skip_update_phase = true;
        if (priority_queue_update(timers->q, timer.id, &now, &timer))
            return timer.id;
        return 0;
    }
}

#define LOCK_TIMERS(timers)                                                   \
    do {                                                                      \
        pthread_mutex_lock(&timers.lock);                                     \
        if (timers.active.is_active                                           \
            && !pthread_equal(pthread_self(), timers.active.thread))          \
            pthread_cond_wait(&timers.cond, &timers.lock);                    \
    } while (0);

#define UNLOCK_TIMERS(timers)                                                 \
    do {                                                                      \
        bool should_wakeup = !timers.active.is_active;                        \
        pthread_mutex_unlock(&timers.lock);                                   \
        if (should_wakeup) io_poller_wakeup(timers.io);                       \
    } while (0)

unsigned
_discord_timer_ctl(struct discord *client,
                   struct discord_timers *timers,
                   struct discord_timer *timer_ret)

{
    LOCK_TIMERS((*timers));
    unsigned id = _discord_timer_ctl_no_lock(client, timers, timer_ret);
    UNLOCK_TIMERS((*timers));
    return id;
}

#define TIMER_TRY_DELETE                                                      \
    if (timer.flags & DISCORD_TIMER_DELETE) {                                 \
        priority_queue_del(timers->q, timer.id);                              \
        timers->active.skip_update_phase = false;                             \
        continue;                                                             \
    }

void
discord_timers_run(struct discord *client, struct discord_timers *timers)
{
    int64_t now = (int64_t)discord_timestamp_us(client);
    const int64_t start_time = now;

    pthread_mutex_lock(&timers->lock);
    timers->active.is_active = true;
    timers->active.thread = pthread_self();
    struct discord_timer timer;
    timers->active.timer = &timer;

    timers->active.skip_update_phase = false;
    for (int64_t trigger, max_iterations = 100000;
         (timer.id = priority_queue_peek(timers->q, &trigger, &timer))
         && max_iterations > 0;
         max_iterations--)
    {
        // update now timestamp every so often
        if ((max_iterations & 0x1F) == 0) {
            now = (int64_t)discord_timestamp_us(client);
            // break if we've spent too much time running timers
            if (now - start_time > 10000) break;
        }

        // no timers to run
        if (trigger > now || trigger == -1) break;

        if (~timer.flags & DISCORD_TIMER_CANCELED) {
            TIMER_TRY_DELETE;

            if (timer.repeat > 0) timer.repeat--;
        }
        if (timer.cb) {
            discord_ev_timer cb = timer.cb;
            pthread_mutex_unlock(&timers->lock);
            cb(client, &timer);
            pthread_mutex_lock(&timers->lock);
        }
        if (timers->active.skip_update_phase) {
            timers->active.skip_update_phase = false;
            continue;
        }

        if ((timer.repeat == 0 || timer.flags & DISCORD_TIMER_CANCELED)
            && (timer.flags & DISCORD_TIMER_DELETE_AUTO))
        {
            timer.flags |= DISCORD_TIMER_DELETE;
        }

        TIMER_TRY_DELETE;

        int64_t next = -1;
        if (timer.delay != -1 && timer.interval >= 0 && timer.repeat != 0
            && ~timer.flags & DISCORD_TIMER_CANCELED)
        {
            next =
                ((timer.flags & DISCORD_TIMER_INTERVAL_FIXED) ? trigger : now)
                + ((timer.flags & DISCORD_TIMER_MICROSECONDS)
                       ? timer.interval
                       : timer.interval * 1000);
        }
        timer.flags &= DISCORD_TIMER_ALLOWED_FLAGS;
        priority_queue_update(timers->q, timer.id, &next, &timer);
    }

    timers->active.is_active = false;
    timers->active.timer = NULL;
    pthread_cond_broadcast(&timers->cond);
    pthread_mutex_unlock(&timers->lock);
}

unsigned
discord_timer_ctl(struct discord *client, struct discord_timer *timer)
{
    return _discord_timer_ctl(client, &client->timers.user, timer);
}

unsigned
discord_internal_timer_ctl(struct discord *client, struct discord_timer *timer)
{
    return _discord_timer_ctl(client, &client->timers.internal, timer);
}

static unsigned
_discord_timer(struct discord *client,
               struct discord_timers *timers,
               discord_ev_timer cb,
               void *data,
               int64_t delay)
{
    struct discord_timer timer = {
        .cb = cb,
        .data = data,
        .delay = delay,
        .flags = DISCORD_TIMER_DELETE_AUTO,
    };
    return _discord_timer_ctl(client, timers, &timer);
}

unsigned
discord_timer_interval(struct discord *client,
                       discord_ev_timer cb,
                       void *data,
                       int64_t delay,
                       int64_t interval,
                       int64_t repeat)
{
    struct discord_timer timer = {
        .cb = cb,
        .data = data,
        .delay = delay,
        .interval = interval,
        .repeat = repeat,
        .flags = DISCORD_TIMER_DELETE_AUTO,
    };
    return discord_timer_ctl(client, &timer);
}

unsigned
discord_timer(struct discord *client,
              discord_ev_timer cb,
              void *data,
              int64_t delay)
{
    return _discord_timer(client, &client->timers.user, cb, data, delay);
}

unsigned
discord_internal_timer(struct discord *client,
                       discord_ev_timer cb,
                       void *data,
                       int64_t delay)
{
    return _discord_timer(client, &client->timers.internal, cb, data, delay);
}

bool
discord_timer_get(struct discord *client,
                  unsigned id,
                  struct discord_timer *timer)
{
    if (!id) return 0;
    LOCK_TIMERS(client->timers.user);
    timer->id = priority_queue_get(client->timers.user.q, id, NULL, timer);
    UNLOCK_TIMERS(client->timers.user);
    return timer->id;
}

static void
discord_timer_disable_update_if_active(struct discord_timers *timers,
                                       unsigned id)
{
    if (!timers->active.timer) return;
    if (timers->active.timer->id == id)
        timers->active.skip_update_phase = true;
}

bool
discord_timer_start(struct discord *client, unsigned id)
{
    bool result = 0;
    struct discord_timer timer;
    LOCK_TIMERS(client->timers.user);
    discord_timer_disable_update_if_active(&client->timers.user, id);
    if (priority_queue_get(client->timers.user.q, id, NULL, &timer)) {
        if (timer.delay < 0) timer.delay = 0;
        result =
            _discord_timer_ctl_no_lock(client, &client->timers.user, &timer);
    }
    UNLOCK_TIMERS(client->timers.user);
    return result;
}

bool
discord_timer_stop(struct discord *client, unsigned id)
{
    bool result = 0;
    struct discord_timer timer;
    LOCK_TIMERS(client->timers.user);
    discord_timer_disable_update_if_active(&client->timers.user, id);
    if (priority_queue_get(client->timers.user.q, id, NULL, &timer)) {
        int64_t disabled = -1;
        result = priority_queue_update(client->timers.user.q, id, &disabled,
                                       &timer);
    }
    UNLOCK_TIMERS(client->timers.user);
    return result;
}

static bool
discord_timer_add_flags(struct discord *client,
                        unsigned id,
                        enum discord_timer_flags flags)
{
    bool result = 0;
    struct discord_timer timer;
    LOCK_TIMERS(client->timers.user);
    discord_timer_disable_update_if_active(&client->timers.user, id);
    if (priority_queue_get(client->timers.user.q, id, NULL, &timer)) {
        timer.flags |= flags;
        int64_t run_now = 0;
        result =
            priority_queue_update(client->timers.user.q, id, &run_now, &timer);
    }
    UNLOCK_TIMERS(client->timers.user);
    return result;
}

bool
discord_timer_cancel(struct discord *client, unsigned id)
{
    return discord_timer_add_flags(client, id, DISCORD_TIMER_CANCELED);
}

bool
discord_timer_delete(struct discord *client, unsigned id)
{
    return discord_timer_add_flags(client, id, DISCORD_TIMER_DELETE);
}

bool
discord_timer_cancel_and_delete(struct discord *client, unsigned id)
{
    return discord_timer_add_flags(
        client, id, DISCORD_TIMER_DELETE | DISCORD_TIMER_CANCELED);
}
