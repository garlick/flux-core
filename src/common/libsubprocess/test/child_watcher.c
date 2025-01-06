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
#include <signal.h>
#include <unistd.h>
#include <flux/core.h>

#include "src/common/libtap/tap.h"
#include "ccan/array_size/array_size.h"
#include "ccan/ptrint/ptrint.h"
#include "child_watcher.h"

void watcher_is (flux_watcher_t *w,
                 bool exp_active,
                 bool exp_referenced,
                 const char *name,
                 const char *what)
{
    bool is_active = flux_watcher_is_active (w);
    bool is_referenced = flux_watcher_is_referenced (w);

    ok (is_active == exp_active && is_referenced == exp_referenced,
        "%s %sact%sref after %s",
        name,
        exp_active ? "+" : "-",
        exp_referenced ? "+" : "-",
        what);
    if (is_active != exp_active)
        diag ("%s is unexpectedly %sact", name, is_active ? "+" : "-");
    if (is_referenced != exp_referenced)
        diag ("%s is unexpectedly %sref", name, is_referenced ? "+" : "-");
}

/* Call this on newly created watcher to check start/stop/is_active and
 * ref/unref/is_referenced basics.
 */
void generic_watcher_check (flux_watcher_t *w, const char *name)
{
    /* ref/unref while inactive causes ev_ref/ev_unref to run
     * in the start/stop callbacks
     */
    watcher_is (w, false, true, name, "init");
    flux_watcher_unref (w);
    watcher_is (w, false, false, name, "unref");
    flux_watcher_start (w);
    watcher_is (w, true, false, name, "start");
    flux_watcher_stop (w);
    watcher_is (w, false, false, name, "stop");
    flux_watcher_ref (w);
    watcher_is (w, false, true, name, "ref");

    /* ref/unref while active causes ev_ref/ev_unref to run
     * in the ref/unref callbacks
     */
    flux_watcher_start (w);
    watcher_is (w, true, true, name, "start");
    flux_watcher_unref (w);
    watcher_is (w, true, false, name, "unref");
    flux_watcher_ref (w);
    watcher_is (w, true, true, name, "ref");
    flux_watcher_stop (w);
    watcher_is (w, false, true, name, "stop");
}

static pid_t child_pid = -1;
static void child_cb (flux_reactor_t *r,
                      flux_watcher_t *w,
                      int revents,
                      void *arg)
{
    int rstatus = child_watcher_get_rstatus (w);
    pid_t rpid = child_watcher_get_rpid (w);

    if ((revents & FLUX_POLLERR))
	    BAIL_OUT ("child watcher called with FLUX_POLLERR set");

    ok (WIFSIGNALED (rstatus) && WTERMSIG (rstatus) == SIGHUP,
        "child watcher called with expected rstatus");
    ok (rpid == child_pid,
        "child watcher called with expected rpid");
    flux_watcher_stop (w);
}

static void test_child  (void)
{
    flux_watcher_t *w;
    flux_reactor_t *r;

    child_pid = fork ();
    if (child_pid == 0) {
        pause ();
        _exit (0);
    }

    if (!(r = flux_reactor_create (0)))
	BAIL_OUT ("could not create non-SIGCHLD reactor");
    errno = 0;
    w = child_watcher_create (r, child_pid, child_cb, NULL);
    ok (w == NULL && errno == EINVAL,
        "child watcher failed with EINVAL on non-SIGCHLD reactor");
    flux_reactor_destroy (r);

    ok ((r = flux_reactor_create (FLUX_REACTOR_SIGCHLD)) != NULL,
        "created reactor with SIGCHLD flag");
    w = child_watcher_create (r, child_pid, child_cb, NULL);
    ok (w != NULL,
        "created child watcher");
    generic_watcher_check (w, "child");

    ok (kill (child_pid, SIGHUP) == 0,
        "sent child SIGHUP");
    flux_watcher_start (w);

    ok (flux_reactor_run (r, 0) == 0,
        "reactor ran successfully");
    flux_watcher_destroy (w);
    flux_reactor_destroy (r);
}

int child_count;
int child_errors;
static void children_cb (flux_reactor_t *r,
                         flux_watcher_t *w,
                         int revents,
                         void *arg)
{
    int status = child_watcher_get_rstatus (w);
    pid_t rpid = child_watcher_get_rpid (w);
    pid_t pid = ptr2int (arg);

    if (!WIFEXITED (status) || WEXITSTATUS (status) != 42) {
        diag ("pid %d exited with unexpected status", pid);
        child_errors++;
    }
    if (pid != rpid) {
        diag ("pid %d != rpid %d", pid, rpid);
        child_errors++;
    }
    child_count++;
    flux_watcher_stop (w);
}

static void test_children (void)
{
    flux_reactor_t *r;
    flux_watcher_t *w[4];

    if (!(r = flux_reactor_create (FLUX_REACTOR_SIGCHLD)))
        BAIL_OUT ("could not create SIGCHLD reactor");

    for (int i = 0; i < ARRAY_SIZE (w); i++) {
        pid_t pid = fork ();
        if (pid == 0)
            _exit (42);
        if (!(w[i] = child_watcher_create (r,
                                           pid,
                                           children_cb,
                                           int2ptr (pid))))
            BAIL_OUT ("could not create watcher: %s", strerror (errno));
        flux_watcher_start (w[i]);
    }

    child_count = 0;
    child_errors = 0;
    ok (flux_reactor_run (r, 0) == 0,
        "reactor ran successfully");

    ok (child_count == ARRAY_SIZE (w),
        "all the children were monitored");
    ok (child_errors == 0,
        "all the children exited in the expected way");

    for (int i = 0; i < ARRAY_SIZE (w); i++)
        flux_watcher_destroy (w[i]);

    flux_reactor_destroy (r);
}

int main (int argc, char **argv)
{
    plan (NO_PLAN);

    test_child ();
    test_children ();

    done_testing ();
}

// vi:ts=4 sw=4 expandtab
