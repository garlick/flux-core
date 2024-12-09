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
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <uv.h>
#include <flux/core.h>

#include "reactor_private.h"

struct flux_reactor {
    uv_loop_t loop;
    int usecount;
    unsigned int errflag:1;
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
        (void)uv_loop_close (&r->loop); // could return -EBUSY
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

flux_reactor_t *flux_reactor_create (int flags)
{
    flux_reactor_t *r;
    int uverr;

    if (valid_flags (flags, FLUX_REACTOR_SIGCHLD) < 0)
        return NULL;
    if (!(r = calloc (1, sizeof (*r))))
        return NULL;
    uverr = uv_loop_init (&r->loop);
    if (uverr < 0) {
        free (r);
        errno = -uverr;
        return NULL;
    }
    r->usecount = 1;
    return r;
}

int flux_reactor_run (flux_reactor_t *r, int flags)
{
    uv_run_mode mode;
    int count;

    if (flags == FLUX_REACTOR_NOWAIT)
        mode = UV_RUN_NOWAIT;
    else if (flags == FLUX_REACTOR_ONCE)
        mode = UV_RUN_ONCE;
    else if (flags == 0)
        mode = UV_RUN_DEFAULT;
    else {
        errno = EINVAL;
        return-1;
    }
    r->errflag = 0;
    count = uv_run (&r->loop, mode);
    return (r->errflag ? -1 : count);
}

double flux_reactor_time (void)
{
    return 1E-9 * uv_hrtime ();
}

double flux_reactor_now (flux_reactor_t *r)
{
    return 1E-3 * uv_now (&r->loop);
}

void flux_reactor_now_update (flux_reactor_t *r)
{
    uv_update_time (&r->loop);
}

void flux_reactor_stop (flux_reactor_t *r)
{
    r->errflag = 0;
    uv_stop (&r->loop);
}

void flux_reactor_stop_error (flux_reactor_t *r)
{
    r->errflag = 1;
    uv_stop (&r->loop);
}

void flux_reactor_active_incref (flux_reactor_t *r)
{
    // FIXME - see https://docs.libuv.org/en/v1.x/handle.html#refcount
}

void flux_reactor_active_decref (flux_reactor_t *r)
{
    // FIXME
}

void *reactor_get_loop (flux_reactor_t *r)
{
    return r ? &r->loop : NULL;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
