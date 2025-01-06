/************************************************************\
 * Copyright 2025 Lawrence Livermore National Security, LLC
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
#include <stdbool.h>
#include <flux/core.h>

#include "src/common/libczmqcontainers/czmq_containers.h"
#include "src/common/libutil/errno_safe.h"
#include "src/common/libflux/watcher_private.h"
#include "src/common/libflux/reactor_private.h"

#include "child_watcher.h"

static zhashx_t *cw_hash; // pid => child watcher

struct child_watcher {
    flux_watcher_t *prepare_w;
    flux_watcher_t *check_w;
    flux_watcher_t *idle_w;
    pid_t pid;
    int status;
    int revents;
    bool running;
};

static size_t cw_hasher (const void *key)
{
    const pid_t *pid = key;
    return *pid;
}

#define NUMCMP(a,b) ((a)==(b)?0:((a)<(b)?-1:1))

static int cw_key_cmp (const void *key1, const void *key2)
{
    const pid_t *pid1 = key1;
    const pid_t *pid2 = key2;

    return NUMCMP (*pid1, *pid2);
}

static void sigchld_cb (pid_t pid, int status, void *arg)
{
    struct child_watcher *cw;

    if (!cw_hash || !(cw = zhashx_lookup (cw_hash, &pid)))
        return;
    cw->status = status;
    cw->revents |= FLUX_POLLIN;
}

static void cw_hash_disable (flux_reactor_t *r)
{
    reactor_sigchld_unregister (r);
    zhashx_destroy (&cw_hash);
}

static int cw_hash_enable (flux_reactor_t *r)
{
    if (reactor_sigchld_register (r, sigchld_cb, NULL) < 0
        || !(cw_hash = zhashx_new ())) {
        cw_hash_disable (r);
        return -1;
    }
    zhashx_set_key_hasher (cw_hash, cw_hasher);
    zhashx_set_key_comparator (cw_hash, cw_key_cmp);
    zhashx_set_key_duplicator (cw_hash, NULL);
    zhashx_set_key_destructor (cw_hash, NULL);
    return 0;
}

static int cw_hash_add (flux_reactor_t *r, struct child_watcher *cw)
{
    if (!cw_hash && cw_hash_enable (r) < 0)
        return -1;
    if (zhashx_insert (cw_hash, &cw->pid, cw) < 0) {
        if (zhashx_size (cw_hash) == 0)
            cw_hash_disable (r);
        return -1;
    }
    return 0;
}

static void cw_hash_delete (flux_reactor_t *r, struct child_watcher *cw)
{
    if (cw_hash) {
        zhashx_delete (cw_hash, &cw->pid);
        if (zhashx_size (cw_hash) == 0)
            cw_hash_disable (r);
    }
}

static void child_watcher_start (flux_watcher_t *w)
{
    struct child_watcher *cw = watcher_get_data (w);
    flux_reactor_t *r = watcher_get_reactor (w);

    if (!cw->running) {
        flux_watcher_start (cw->prepare_w);
        flux_watcher_start (cw->check_w);

        if (cw_hash_add (r, cw) < 0)
            cw->revents |= FLUX_POLLERR;

        cw->running = true;
    }
}

static void child_watcher_stop (flux_watcher_t *w)
{
    struct child_watcher *cw = watcher_get_data (w);
    flux_reactor_t *r = watcher_get_reactor (w);

    if (cw->running) {
        flux_watcher_stop (cw->prepare_w);
        flux_watcher_stop (cw->check_w);
        flux_watcher_stop (cw->idle_w);

        cw_hash_delete (r, cw);
        cw->running = false;
    }
}

static void child_watcher_ref (flux_watcher_t *w)
{
    struct child_watcher *cw = watcher_get_data (w);

    flux_watcher_ref (cw->prepare_w);
    flux_watcher_ref (cw->idle_w);
    flux_watcher_ref (cw->check_w);
}

static void child_watcher_unref (flux_watcher_t *w)
{
    struct child_watcher *cw = watcher_get_data (w);

    flux_watcher_unref (cw->prepare_w);
    flux_watcher_unref (cw->idle_w);
    flux_watcher_unref (cw->check_w);
}

static bool child_watcher_is_active (flux_watcher_t *w)
{
    struct child_watcher *cw = watcher_get_data (w);

    return cw->running;
}

static void child_watcher_destroy (flux_watcher_t *w)
{
    struct child_watcher *cw = watcher_get_data (w);

    if (cw) {
        flux_watcher_destroy (cw->prepare_w);
        flux_watcher_destroy (cw->check_w);
        flux_watcher_destroy (cw->idle_w);
    }
}

static void child_watcher_prepare_cb (flux_reactor_t *r,
                                      flux_watcher_t *prepare_w,
                                      int prepare_revents,
                                      void *arg)
{
    flux_watcher_t *w = arg;
    struct child_watcher *cw = watcher_get_data (w);

    if (cw->revents)
        flux_watcher_start (cw->idle_w);
}

static void child_watcher_check_cb (flux_reactor_t *r,
                                    flux_watcher_t *check_w,
                                    int check_revents,
                                    void *arg)
{
    flux_watcher_t *w = arg;
    struct child_watcher *cw = watcher_get_data (w);

    flux_watcher_stop (cw->idle_w);
    if (cw->revents) {
        watcher_call (w, cw->revents);
        cw->revents = 0;
    }
}

static struct flux_watcher_ops child_watcher_ops = {
    .start = child_watcher_start,
    .stop = child_watcher_stop,
    .ref = child_watcher_ref,
    .unref = child_watcher_unref,
    .is_active = child_watcher_is_active,
    .destroy = child_watcher_destroy,
};

/* N.B. unlike libev's ev_child, these watchers do not accept pid=0 to
 * watch any child, nor do they have a 'trace' flag.
 */
flux_watcher_t *child_watcher_create (flux_reactor_t *r,
                                      int pid,
                                      flux_watcher_f cb,
                                      void *arg)
{
    struct child_watcher *cw;
    flux_watcher_t *w;

    if (!(reactor_get_flags (r) & FLUX_REACTOR_SIGCHLD) || pid <= 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!(w = watcher_create (r, sizeof (*cw), &child_watcher_ops, cb, arg)))
        return NULL;
    cw = watcher_get_data (w);
    cw->pid = pid;

    if (!(cw->prepare_w = flux_prepare_watcher_create (r,
                                                       child_watcher_prepare_cb,
                                                       w))
        || !(cw->check_w = flux_check_watcher_create (r,
                                                      child_watcher_check_cb,
                                                      w))
        || !(cw->idle_w = flux_idle_watcher_create (r, NULL, NULL)))
        goto error;
    return w;
error:
    ERRNO_SAFE_WRAP (flux_watcher_destroy, w);
    return NULL;
}

int child_watcher_get_rstatus (flux_watcher_t *w)
{
    if (watcher_get_ops (w) != &child_watcher_ops) {
        errno = EINVAL;
        return -1;
    }
    struct child_watcher *cw = watcher_get_data (w);
    return cw->status;
}

pid_t child_watcher_get_rpid (flux_watcher_t *w)
{
    if (watcher_get_ops (w) != &child_watcher_ops) {
        errno = EINVAL;
        return -1;
    }
    struct child_watcher *cw = watcher_get_data (w);
    return cw->pid;
}


// vi:ts=4 sw=4 expandtab
