/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* watcher_wrap.c - wrapped libuv watchers */

/* CAVEAT: a libuv handle (the libuv name for watcher) cannot be directly
 * destroyed.  In the Flux watcher destroy callback, we call uv_close(),
 * registering libuv_close_cb(), then in libuv_close_cb(), the handle is freed.
 * If the reactor doesn't run to allow the callback to run, memory is leaked.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <uv.h>
#include <flux/core.h>

#include "src/common/libutil/errno_safe.h"

#include "reactor_private.h"
#include "watcher_private.h"

static inline int events_to_libuv (int events)
{
    int e = 0;
    if (events & FLUX_POLLIN)
        e |= UV_READABLE;
    if (events & FLUX_POLLOUT)
        e |= UV_WRITABLE;
    if (events & FLUX_POLLERR)
        e |= UV_DISCONNECT;
    return e;
}

static inline int libuv_to_events (int events)
{
    int e = 0;
    if (events & UV_READABLE)
        e |= FLUX_POLLIN;
    if (events & UV_WRITABLE)
        e |= FLUX_POLLOUT;
    if (events & UV_DISCONNECT)
        e |= FLUX_POLLERR;
    return e;
}

static void libuv_close_cb (uv_handle_t *uvh)
{
    free (uvh);
}

static void watcher_call_uv (flux_watcher_t *w, int revents)
{
    watcher_call (w, libuv_to_events (revents));
}

/* file descriptors
 */

struct fdwatcher {
    uv_poll_t *uvh;
    int revents;
};

static void fdwatcher_cb (uv_poll_t *uvh, int status, int events)
{
    struct flux_watcher *w = uv_handle_get_data ((uv_handle_t *)uvh);
    if (status < 0)
        watcher_call_uv (w, FLUX_POLLERR);
    else
        watcher_call_uv (w, events);
 }

static void fdwatcher_start (flux_watcher_t *w)
{
    struct fdwatcher *fdw = watcher_get_data (w);
    uv_poll_start (fdw->uvh, fdw->revents, fdwatcher_cb);
}

static void fdwatcher_stop (flux_watcher_t *w)
{
    struct fdwatcher *fdw = watcher_get_data (w);
    uv_poll_stop (fdw->uvh);
}

static bool fdwatcher_is_active (flux_watcher_t *w)
{
    struct fdwatcher *fdw = watcher_get_data (w);
    return uv_is_active ((uv_handle_t *)fdw->uvh);
}

static void fdwatcher_destroy (flux_watcher_t *w)
{
    struct fdwatcher *fdw = watcher_get_data (w);
    uv_close ((uv_handle_t *)fdw->uvh, libuv_close_cb);
}

static struct flux_watcher_ops fdwatcher_ops = {
    .start = fdwatcher_start,
    .stop = fdwatcher_stop,
    .is_active = fdwatcher_is_active,
    .destroy = fdwatcher_destroy,
};

flux_watcher_t *flux_fd_watcher_create (flux_reactor_t *r,
                                        int fd,
                                        int events,
                                        flux_watcher_f cb,
                                        void *arg)
{
    uv_loop_t *loop = reactor_get_loop (r);
    struct fdwatcher *fdw;
    flux_watcher_t *w;

    if (!(w = watcher_create (r, sizeof (*fdw), &fdwatcher_ops, cb, arg)))
        return NULL;
    fdw = watcher_get_data (w);
    fdw->revents = events_to_libuv (events);
    if (!(fdw->uvh = calloc (1, sizeof (*fdw->uvh))))
        goto error;
    uv_poll_init (loop, fdw->uvh, fd);
    uv_handle_set_data ((uv_handle_t *)fdw->uvh, w);

    return w;
error:
    flux_watcher_destroy (w);
    return NULL;
}

int flux_fd_watcher_get_fd (flux_watcher_t *w)
{
    if (watcher_get_ops (w) != &fdwatcher_ops) {
        errno = EINVAL;
        return -1;
    }
    struct fdwatcher *fdw = watcher_get_data (w);
    int uverr;
    int fd;
    if ((uverr = uv_fileno ((uv_handle_t *)fdw->uvh, &fd)) < 0) {
        errno = -uverr;
        return -1;
    }
    return fd;
}

/* Timer
 */

struct tmwatcher {
    uv_timer_t *uvh;
    uint64_t timeout;
    uint64_t repeat;
};

static void tmwatcher_cb (uv_timer_t *uvh)
{
    struct flux_watcher *w = uv_handle_get_data ((uv_handle_t *)uvh);
    watcher_call_uv (w, 0);
}

static void tmwatcher_start (flux_watcher_t *w)
{
    struct tmwatcher *tmw = watcher_get_data (w);
    uv_timer_start (tmw->uvh, tmwatcher_cb, tmw->timeout, tmw->repeat);
}

static void tmwatcher_stop (flux_watcher_t *w)
{
    struct tmwatcher *tmw = watcher_get_data (w);
    uv_timer_stop (tmw->uvh);
}

static bool tmwatcher_is_active (flux_watcher_t *w)
{
    struct tmwatcher *tmw = watcher_get_data (w);
    return uv_is_active ((uv_handle_t *)tmw->uvh);
}

static void tmwatcher_destroy (flux_watcher_t *w)
{
    struct tmwatcher *tmw = watcher_get_data (w);
    uv_close ((uv_handle_t *)tmw->uvh, libuv_close_cb);
}

static struct flux_watcher_ops tmwatcher_ops = {
    .start = tmwatcher_start,
    .stop = tmwatcher_stop,
    .is_active = tmwatcher_is_active,
    .destroy = tmwatcher_destroy,
};

flux_watcher_t *flux_timer_watcher_create (flux_reactor_t *r,
                                           double after,
                                           double repeat,
                                           flux_watcher_f cb,
                                           void *arg)
{
    uv_loop_t *loop = reactor_get_loop (r);
    struct tmwatcher *tmw;
    flux_watcher_t *w;

    if (after < 0 || repeat < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!(w = watcher_create (r, sizeof (*tmw), &tmwatcher_ops, cb, arg)))
        return NULL;
    tmw = watcher_get_data (w);
    tmw->timeout = 1000ULL * after;
    tmw->repeat = 1000ULL * repeat;
    if (!(tmw->uvh = calloc (1, sizeof (*tmw->uvh))))
        goto error;
    uv_timer_init (loop, tmw->uvh);
    uv_handle_set_data ((uv_handle_t *)tmw->uvh, w);

    return w;
error:
    flux_watcher_destroy (w);
    return NULL;
}

void flux_timer_watcher_reset (flux_watcher_t *w, double after, double repeat)
{
    if (watcher_get_ops (w) != &tmwatcher_ops)
        return;
    struct tmwatcher *tmw = watcher_get_data (w);
    tmw->timeout = 1000ULL * after;
    tmw->repeat = 1000ULL * repeat;
}

void flux_timer_watcher_again (flux_watcher_t *w)
{
    if (watcher_get_ops (w) != &tmwatcher_ops)
        return;
    struct tmwatcher *tmw = watcher_get_data (w);
    /* in future.c::then_context_set_timeout() we assume that 'again' can be
     * run on a timer that hasn't been started.  That was apparently allowed
     * by libev, but is not allowed by libev
     */
    if (uv_timer_again (tmw->uvh) == UV_EINVAL) {
        if (tmw->repeat > 0)
            flux_watcher_start (w);
    }
}

#if TODO_PERIODIC
/* Periodic
 */

static void safe_stop_cb (struct ev_loop *loop, ev_prepare *pw, int revents)
{
    flux_watcher_stop ((flux_watcher_t *)pw->data);
    ev_prepare_stop (loop, pw);
    free (pw);
}

/* Stop a watcher in the next ev_prepare callback. To be used from periodics
 *  reschedule callback or other ev callbacks in which it is documented
 *  unsafe to modify the ev_loop or any watcher.
 */
static void watcher_stop_safe (flux_watcher_t *w)
{
    if (w) {
        ev_prepare *pw = calloc (1, sizeof (*pw));
        if (!pw) /* On ENOMEM, we just have to give up */
            return;
        ev_prepare_init (pw, safe_stop_cb);
        pw->data = w;
        ev_prepare_start (watcher_get_ev (w), pw);
    }
}
struct f_periodic {
    struct flux_watcher *w;
    ev_periodic          evp;
    flux_reschedule_f    reschedule_cb;
};

static void periodic_start (flux_watcher_t *w)
{
    struct ev_loop *loop = watcher_get_ev (w);
    struct f_periodic *fp = watcher_get_data (w);
    ev_periodic_start (loop, &fp->evp);
}

static void periodic_stop (flux_watcher_t *w)
{
    struct ev_loop *loop = watcher_get_ev (w);
    struct f_periodic *fp = watcher_get_data (w);
    ev_periodic_stop (loop, &fp->evp);
}

static bool periodic_is_active (flux_watcher_t *w)
{
    struct f_periodic *fp = watcher_get_data (w);
    return ev_is_active (&fp->evp);
}

static void periodic_cb (struct ev_loop *loop, ev_periodic *pw, int revents)
{
    struct f_periodic *fp = pw->data;
    struct flux_watcher *w = fp->w;
    watcher_call_ev (w, revents);
}

static ev_tstamp periodic_reschedule_cb (ev_periodic *pw, ev_tstamp now)
{
    ev_tstamp rc;
    struct f_periodic *fp = pw->data;
    assert (fp->reschedule_cb != NULL);
    rc = (ev_tstamp)fp->reschedule_cb (fp->w,
                                       (double)now,
                                       watcher_get_arg (fp->w));
    if (rc < now) {
        /*  User reschedule cb returned time in the past. The watcher will
         *   be stopped, but not here (changing loop is not allowed in a
         *   libev reschedule cb. flux_watcher_stop_safe() will stop it in
         *   a prepare callback.
         *  Return time far in the future to ensure we aren't called again.
         */
        watcher_stop_safe (fp->w);
        return (now + 1e99);
    }
    return rc;
}

static struct flux_watcher_ops periodic_watcher = {
    .start = periodic_start,
    .stop = periodic_stop,
    .destroy = NULL,
    .is_active = periodic_is_active,
};

flux_watcher_t *flux_periodic_watcher_create (flux_reactor_t *r,
                                              double offset,
                                              double interval,
                                              flux_reschedule_f reschedule_cb,
                                              flux_watcher_f cb,
                                              void *arg)
{
    flux_watcher_t *w;
    struct f_periodic *fp;
    size_t size = sizeof (*fp);
    if (offset < 0 || interval < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!(w = watcher_create (r, size, &periodic_watcher, cb, arg)))
        return NULL;
    fp = watcher_get_data (w);
    fp->evp.data = fp;
    fp->w = w;
    fp->reschedule_cb = reschedule_cb;

    ev_periodic_init (&fp->evp,
                      periodic_cb,
                      offset,
                      interval,
                      reschedule_cb ? periodic_reschedule_cb : NULL);

    return w;
}

void flux_periodic_watcher_reset (flux_watcher_t *w,
                                  double next,
                                  double interval,
                                  flux_reschedule_f reschedule_cb)
{
    struct ev_loop *loop = watcher_get_ev (w);
    struct f_periodic *fp = watcher_get_data (w);
    assert (watcher_get_ops (w) == &periodic_watcher);
    fp->reschedule_cb = reschedule_cb;
    ev_periodic_set (&fp->evp,
                     next,
                     interval,
                     reschedule_cb ? periodic_reschedule_cb : NULL);
    ev_periodic_again (loop, &fp->evp);
}

double flux_watcher_next_wakeup (flux_watcher_t *w)
{
    if (watcher_get_ops (w) == &periodic_watcher) {
        struct f_periodic *fp = watcher_get_data (w);
        return ((double) ev_periodic_at (&fp->evp));
    }
    else if (watcher_get_ops (w) == &timer_watcher) {
        ev_timer *tw = watcher_get_data (w);
        struct ev_loop *loop = watcher_get_ev (w);
        return ((double) (ev_now (loop) +  ev_timer_remaining (loop, tw)));
    }
    errno = EINVAL;
    return  (-1.);
}
#endif

/* Prepare
 */

struct prepwatcher {
    uv_prepare_t *uvh;
};

static void prepwatcher_cb (uv_prepare_t *uvh)
{
    struct flux_watcher *w = uv_handle_get_data ((uv_handle_t *)uvh);
    watcher_call_uv (w, 0);
}

static void prepwatcher_start (flux_watcher_t *w)
{
    struct prepwatcher *pw = watcher_get_data (w);
    uv_prepare_start (pw->uvh, prepwatcher_cb);
}

static void prepwatcher_stop (flux_watcher_t *w)
{
    struct prepwatcher *pw = watcher_get_data (w);
    uv_prepare_stop (pw->uvh);
}

static bool prepwatcher_is_active (flux_watcher_t *w)
{
    struct prepwatcher *pw = watcher_get_data (w);
    return uv_is_active ((uv_handle_t *)pw->uvh);
}

static void prepwatcher_destroy (flux_watcher_t *w)
{
    struct prepwatcher *pw = watcher_get_data (w);
    uv_close ((uv_handle_t *)pw->uvh, libuv_close_cb);
}

static struct flux_watcher_ops prepwatcher_ops = {
    .start = prepwatcher_start,
    .stop = prepwatcher_stop,
    .is_active = prepwatcher_is_active,
    .destroy = prepwatcher_destroy,
};

flux_watcher_t *flux_prepare_watcher_create (flux_reactor_t *r,
                                             flux_watcher_f cb,
                                             void *arg)
{
    uv_loop_t *loop = reactor_get_loop (r);
    struct prepwatcher *pw;
    flux_watcher_t *w;

    if (!(w = watcher_create (r, sizeof (*pw), &prepwatcher_ops, cb, arg)))
        return NULL;
    pw = watcher_get_data (w);
    if (!(pw->uvh = calloc (1, sizeof (*pw->uvh))))
        goto error;
    uv_prepare_init (loop, pw->uvh);
    uv_handle_set_data ((uv_handle_t *)pw->uvh, w);

    return w;
error:
    flux_watcher_destroy (w);
    return NULL;
}

/* Check
 */

struct chkwatcher {
    uv_check_t *uvh;
};

static void chkwatcher_cb (uv_check_t *uvh)
{
    struct flux_watcher *w = uv_handle_get_data ((uv_handle_t *)uvh);
    watcher_call_uv (w, 0);
}

static void chkwatcher_start (flux_watcher_t *w)
{
    struct chkwatcher *cw = watcher_get_data (w);
    uv_check_start (cw->uvh, chkwatcher_cb);
}

static void chkwatcher_stop (flux_watcher_t *w)
{
    struct chkwatcher *cw = watcher_get_data (w);
    uv_check_stop (cw->uvh);
}

static bool chkwatcher_is_active (flux_watcher_t *w)
{
    struct chkwatcher *cw = watcher_get_data (w);
    return uv_is_active ((uv_handle_t *)cw->uvh);
}

static void chkwatcher_destroy (flux_watcher_t *w)
{
    struct chkwatcher *cw = watcher_get_data (w);
    uv_close ((uv_handle_t *)cw->uvh, libuv_close_cb);
}

static struct flux_watcher_ops chkwatcher_ops = {
    .start = chkwatcher_start,
    .stop = chkwatcher_stop,
    .is_active = chkwatcher_is_active,
    .destroy = chkwatcher_destroy,
};

flux_watcher_t *flux_check_watcher_create (flux_reactor_t *r,
                                           flux_watcher_f cb,
                                           void *arg)
{
    uv_loop_t *loop = reactor_get_loop (r);
    struct chkwatcher *cw;
    flux_watcher_t *w;

    if (!(w = watcher_create (r, sizeof (*cw), &chkwatcher_ops, cb, arg)))
        return NULL;
    cw = watcher_get_data (w);
    if (!(cw->uvh = calloc (1, sizeof (*cw->uvh))))
        goto error;
    uv_check_init (loop, cw->uvh);
    uv_handle_set_data ((uv_handle_t *)cw->uvh, w);

    return w;
error:
    flux_watcher_destroy (w);
    return NULL;
}

/* Idle
 */
struct idlewatcher {
    uv_idle_t *uvh;
};

static void idlewatcher_cb (uv_idle_t *uvh)
{
    struct flux_watcher *w = uv_handle_get_data ((uv_handle_t *)uvh);
    watcher_call_uv (w, 0);
}

static void idlewatcher_start (flux_watcher_t *w)
{
    struct idlewatcher *iw = watcher_get_data (w);
    uv_idle_start (iw->uvh, idlewatcher_cb);
}

static void idlewatcher_stop (flux_watcher_t *w)
{
    struct idlewatcher *iw = watcher_get_data (w);
    uv_idle_stop (iw->uvh);
}

static bool idlewatcher_is_active (flux_watcher_t *w)
{
    struct idlewatcher *iw = watcher_get_data (w);
    return uv_is_active ((uv_handle_t *)iw->uvh);
}

static void idlewatcher_destroy (flux_watcher_t *w)
{
    struct idlewatcher *iw = watcher_get_data (w);
    uv_close ((uv_handle_t *)iw->uvh, libuv_close_cb);
}

static struct flux_watcher_ops idlewatcher_ops = {
    .start = idlewatcher_start,
    .stop = idlewatcher_stop,
    .is_active = idlewatcher_is_active,
    .destroy = idlewatcher_destroy,
};

flux_watcher_t *flux_idle_watcher_create (flux_reactor_t *r,
                                          flux_watcher_f cb,
                                          void *arg)
{
    uv_loop_t *loop = reactor_get_loop (r);
    struct idlewatcher *iw;
    flux_watcher_t *w;

    if (!(w = watcher_create (r, sizeof (*iw), &idlewatcher_ops, cb, arg)))
        return NULL;
    iw = watcher_get_data (w);
    if (!(iw->uvh = calloc (1, sizeof (*iw->uvh))))
        goto error;
    uv_idle_init (loop, iw->uvh);
    uv_handle_set_data ((uv_handle_t *)iw->uvh, w);

    return w;
error:
    flux_watcher_destroy (w);
    return NULL;
}

#if TODO_CHILD
/* Child
 */

static void child_start (flux_watcher_t *w)
{
    struct ev_loop *loop = watcher_get_ev (w);
    ev_child *cw = watcher_get_data (w);
    ev_child_start (loop, cw);
}

static void child_stop (flux_watcher_t *w)
{
    struct ev_loop *loop = watcher_get_ev (w);
    ev_child *cw = watcher_get_data (w);
    ev_child_stop (loop, cw);
}

static void child_cb (struct ev_loop *loop, ev_child *cw, int revents)
{
    struct flux_watcher *w = cw->data;
    watcher_call_ev (w, revents);
}

static struct flux_watcher_ops child_watcher = {
    .start = child_start,
    .stop = child_stop,
    .destroy = NULL,
    .is_active = wrap_ev_active,
};


flux_watcher_t *flux_child_watcher_create (flux_reactor_t *r,
                                           int pid,
                                           bool trace,
                                           flux_watcher_f cb,
                                           void *arg)
{
    flux_watcher_t *w;
    ev_child *cw;

    if (!ev_is_default_loop (reactor_get_loop (r))) {
        errno = EINVAL;
        return NULL;
    }
    if (!(w = watcher_create (r, sizeof (*cw), &child_watcher, cb, arg)))
        return NULL;
    cw = watcher_get_data (w);
    ev_child_init (cw, child_cb, pid, trace ? 1 : 0);
    cw->data = w;

    return w;
}

int flux_child_watcher_get_rpid (flux_watcher_t *w)
{
    if (watcher_get_ops (w) != &child_watcher) {
        errno = EINVAL;
        return -1;
    }
    ev_child *cw = watcher_get_data (w);
    return cw->rpid;
}

int flux_child_watcher_get_rstatus (flux_watcher_t *w)
{
    if (watcher_get_ops (w) != &child_watcher) {
        errno = EINVAL;
        return -1;
    }
    ev_child *cw = watcher_get_data (w);
    return cw->rstatus;
}
#endif

/* Signal
 */
struct sigwatcher {
    uv_signal_t *uvh;
    int signum;
};

static void sigwatcher_cb (uv_signal_t *uvh, int signum)
{
    struct flux_watcher *w = uv_handle_get_data ((uv_handle_t *)uvh);
    watcher_call_uv (w, 0);
}

static void sigwatcher_start (flux_watcher_t *w)
{
    struct sigwatcher *sw = watcher_get_data (w);
    uv_signal_start (sw->uvh, sigwatcher_cb, sw->signum);
}

static void sigwatcher_stop (flux_watcher_t *w)
{
    struct sigwatcher *sw = watcher_get_data (w);
    uv_signal_stop (sw->uvh);
}

static bool sigwatcher_is_active (flux_watcher_t *w)
{
    struct sigwatcher *sw = watcher_get_data (w);
    return uv_is_active ((uv_handle_t *)sw->uvh);
}

static void sigwatcher_destroy (flux_watcher_t *w)
{
    struct sigwatcher *sw = watcher_get_data (w);
    uv_close ((uv_handle_t *)sw->uvh, libuv_close_cb);
}

static struct flux_watcher_ops sigwatcher_ops = {
    .start = sigwatcher_start,
    .stop = sigwatcher_stop,
    .is_active = sigwatcher_is_active,
    .destroy = sigwatcher_destroy,
};

flux_watcher_t *flux_signal_watcher_create (flux_reactor_t *r,
                                            int signum,
                                            flux_watcher_f cb,
                                            void *arg)
{
    uv_loop_t *loop = reactor_get_loop (r);
    flux_watcher_t *w;
    struct sigwatcher *sw;

    if (!(w = watcher_create (r, sizeof (*sw), &sigwatcher_ops, cb, arg)))
        return NULL;
    sw = watcher_get_data (w);
    sw->signum = signum;
    if (!(sw->uvh = calloc (1, sizeof (*sw->uvh))))
        goto error;
    uv_signal_init (loop, sw->uvh);
    uv_handle_set_data ((uv_handle_t *)sw->uvh, w);

    return w;
error:
    flux_watcher_destroy (w);
    return NULL;
}

int flux_signal_watcher_get_signum (flux_watcher_t *w)
{
    if (watcher_get_ops (w) != &sigwatcher_ops) {
        errno = EINVAL;
        return -1;
    }
    struct sigwatcher *sw = watcher_get_data (w);
    return sw->uvh->signum;
}

/* Stat
 */

struct statwatcher {
    uv_fs_event_t *uvh;
    char *path;
    struct stat prev;
    struct stat stat;
};

static void statwatcher_cb (uv_fs_event_t *uvh,
                            const char *filename,
                            int events,
                            int status)
{
    struct flux_watcher *w = uv_handle_get_data ((uv_handle_t *)uvh);
    struct statwatcher *sw = watcher_get_data (w);
    sw->prev = sw->stat;
    if (stat (sw->path, &sw->stat) < 0)
        sw->stat.st_nlink = 0;
    watcher_call_uv (w, 0);
}

static void statwatcher_start (flux_watcher_t *w)
{
    struct statwatcher *sw = watcher_get_data (w);
    uv_fs_event_start (sw->uvh,
                       statwatcher_cb,
                       sw->path,
                       UV_FS_EVENT_WATCH_ENTRY);
}

static void statwatcher_stop (flux_watcher_t *w)
{
    struct statwatcher *sw = watcher_get_data (w);
    uv_fs_event_stop (sw->uvh);
}

static bool statwatcher_is_active (flux_watcher_t *w)
{
    struct statwatcher *sw = watcher_get_data (w);
    return uv_is_active ((uv_handle_t *)sw->uvh);
}

static void statwatcher_destroy (flux_watcher_t *w)
{
    struct statwatcher *sw = watcher_get_data (w);
    uv_close ((uv_handle_t *)sw->uvh, libuv_close_cb);
    ERRNO_SAFE_WRAP (free, sw->path);
}

static struct flux_watcher_ops statwatcher_ops = {
    .start = statwatcher_start,
    .stop = statwatcher_stop,
    .is_active = statwatcher_is_active,
    .destroy = statwatcher_destroy,
};

flux_watcher_t *flux_stat_watcher_create (flux_reactor_t *r,
                                          const char *path,
                                          double interval, // ignored
                                          flux_watcher_f cb,
                                          void *arg)
{
    uv_loop_t *loop = reactor_get_loop (r);
    flux_watcher_t *w;
    struct statwatcher *sw;

    if (!(w = watcher_create (r, sizeof (*sw), &statwatcher_ops, cb, arg)))
        return NULL;
    sw = watcher_get_data (w);
    if (!(sw->path = strdup (path))
        || !(sw->uvh = calloc (1, sizeof (*(sw->uvh)))))
        goto error;
    if (stat (path, &sw->stat) < 0)
        sw->stat.st_nlink = 0;
    sw->prev = sw->stat;
    uv_fs_event_init (loop, sw->uvh);
    uv_handle_set_data ((uv_handle_t *)sw->uvh, w);

    return w;
error:
    flux_watcher_destroy (w);
    return NULL;
}

void flux_stat_watcher_get_rstat (flux_watcher_t *w,
                                  struct stat *stat,
                                  struct stat *prev)
{
    if (watcher_get_ops (w) != &statwatcher_ops)
        return;
    struct statwatcher *sw = watcher_get_data (w);
    if (stat)
        *stat = sw->stat;
    if (prev)
        *prev = sw->prev;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
