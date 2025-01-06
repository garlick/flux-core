/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <ev.h>
#include <flux/core.h>

#include "reactor_private.h"

struct flux_reactor {
    struct ev_loop *loop;
    int usecount;
    unsigned int errflag:1;
    int flags;

    flux_watcher_t *sigchld_w;
    sigchld_f sigchld_cb;
    void *sigchld_arg;
};

static int valid_flags (int flags, int valid)
{
    if ((flags & ~valid)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void flux_reactor_decref (flux_reactor_t *r)
{
    if (r && --r->usecount == 0) {
        int saved_errno = errno;
        // N.B. decrefs reactor but no double free if r->usecount == -1
        flux_watcher_destroy (r->sigchld_w);
        ev_loop_destroy (r->loop);
        free (r);
        errno = saved_errno;
    }
}

void flux_reactor_incref (flux_reactor_t *r)
{
    if (r)
        r->usecount++;
}

void flux_reactor_destroy (flux_reactor_t *r)
{
    flux_reactor_decref (r);
}

static void sigchld_cb (flux_reactor_t *r,
                        flux_watcher_t *w,
                        int revents,
                        void *arg)
{
    pid_t pid;
    int status;

    do {
        pid = waitpid (-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid > 0) {
            if (r->sigchld_cb)
                r->sigchld_cb (pid, status, r->sigchld_arg);
        }
    } while (pid > 0);
}

static flux_watcher_t *sigchld_create (flux_reactor_t *r)
{
    flux_watcher_t *w;
    if (!(w = flux_signal_watcher_create (r, SIGCHLD, sigchld_cb, NULL)))
        return NULL;
    flux_watcher_unref (w); // don't prevent loop from exiting
    flux_reactor_decref (r); // don't prevent reactor destruction
    flux_watcher_start (w);
    return w;
}

void reactor_sigchld_unregister (flux_reactor_t *r)
{
    r->sigchld_cb = NULL;
    r->sigchld_arg = NULL;
}

int reactor_sigchld_register (flux_reactor_t *r, sigchld_f cb, void *arg)
{
    if (!r->sigchld_w) {
        errno = EINVAL;
        return -1;
    }
    r->sigchld_cb = cb;
    r->sigchld_arg = arg;
    return 0;
}

int reactor_get_flags (flux_reactor_t *r)
{
    return r ? r->flags : 0;
}

flux_reactor_t *flux_reactor_create (int flags)
{
    flux_reactor_t *r;

    if (valid_flags (flags, FLUX_REACTOR_SIGCHLD) < 0)
        return NULL;
    if (!(r = calloc (1, sizeof (*r))))
        return NULL;
    r->flags = flags;
    r->loop = ev_loop_new (EVFLAG_NOSIGMASK | EVFLAG_SIGNALFD);
    if (!r->loop) {
        errno = ENOMEM;
        goto error;
    }
    ev_set_userdata (r->loop, r);
    r->usecount = 1;
    if ((flags & FLUX_REACTOR_SIGCHLD)) {
        if (!(r->sigchld_w = sigchld_create (r)))
            goto error;
    }
    return r;
error:
    flux_reactor_destroy (r);
    return NULL;
}

int flux_reactor_run (flux_reactor_t *r, int flags)
{
    int ev_flags = 0;
    int count;

    if (valid_flags (flags, FLUX_REACTOR_NOWAIT | FLUX_REACTOR_ONCE) < 0)
        return -1;
    if (flags & FLUX_REACTOR_NOWAIT)
        ev_flags |= EVRUN_NOWAIT;
    if (flags & FLUX_REACTOR_ONCE)
        ev_flags |= EVRUN_ONCE;
    r->errflag = 0;
    count = ev_run (r->loop, ev_flags);
    return (r->errflag ? -1 : count);
}

double flux_reactor_time (void)
{
    return ev_time ();
}

double flux_reactor_now (flux_reactor_t *r)
{
    return ev_now (r->loop);
}

void flux_reactor_now_update (flux_reactor_t *r)
{
    return ev_now_update (r->loop);
}

void flux_reactor_stop (flux_reactor_t *r)
{
    r->errflag = 0;
    ev_break (r->loop, EVBREAK_ALL);
}

void flux_reactor_stop_error (flux_reactor_t *r)
{
    r->errflag = 1;
    ev_break (r->loop, EVBREAK_ALL);
}

void *reactor_get_loop (flux_reactor_t *r)
{
    return r ? r->loop : NULL;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
