/************************************************************\
 * Copyright 2024 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* sysjob - create jobs internally
 *
 * Sysjobs have the following special characteristics:
 * - the FLUX_JOB_SYSTEM flag is set
 * - runs as instance owner
 * - always run one task per node for the given resource set
 * - jobtap disabled
 * - uses primary KVS namespace
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <unistd.h>
#include <jansson.h>
#include <flux/core.h>
#include <flux/idset.h>

#include "src/common/libutil/fluid.h"
#include "src/common/libutil/errno_safe.h"
#include "src/common/libutil/errprintf.h"
#include "src/common/libeventlog/eventlog.h"
#include "src/common/libjob/sign_none.h"
#include "src/common/libsubprocess/command_private.h"
#include "src/common/libsubprocess/bulk-exec.h"
#include "src/common/librlist/rlist.h"

#include "event.h"
#include "journal.h"
#include "sysjob.h"

// N.B. job-ingest leaves ids 16367 - 16383 unused for special needs like this
static int sysjob_fluid_generator_id = 16383;

struct exec_ctx {
    struct job *job;
    char *name;
    struct bulk_exec *bulk_exec;
    bool started;
    struct rlist *rl;       // R diminished each time a subset is released
    struct idset *pending;  // broker ranks
    struct sysjob *sys;
    int status;
};

struct sysjob {
    struct fluid_generator gen;
    bool gen_initialized;
    uint32_t size;
    struct job_manager *ctx;
};

static struct bulk_exec_ops bulk_exec_ops;

static void exec_ctx_destroy (struct exec_ctx *x)
{
    if (x) {
        int saved_errno = errno;
        free (x->name);
        bulk_exec_destroy (x->bulk_exec);
        rlist_destroy (x->rl);
        idset_destroy (x->pending);
        free (x);
        errno = saved_errno;
    }
}

static struct exec_ctx *exec_ctx_create (struct sysjob *sys,
                                         struct job *job,
                                         const char *name,
                                         flux_cmd_t *cmd,
                                         json_t *R)
{
    struct exec_ctx *x;

    if (!(x = calloc (1, sizeof (*x))))
        return NULL;
    x->job = job;
    x->sys = sys;
    if (!(x->name = strdup (name))
        || !(x->bulk_exec = bulk_exec_create (&bulk_exec_ops,
                                              "rexec",
                                              job->id,
                                              name,
                                              x))
        || !(x->rl = rlist_from_json (R, NULL))
        || !(x->pending = rlist_ranks (x->rl))
        || bulk_exec_push_cmd (x->bulk_exec, x->pending, cmd, 0) < 0)
        goto error;
    return x;
error:
    exec_ctx_destroy (x);
    return NULL;
}

static void bulk_start_cb (struct bulk_exec *bx, void *arg)
{
}

static void bulk_exit_cb (struct bulk_exec *bx,
                          void *arg,
                          const struct idset *ranks)
{
}

static void bulk_complete_cb (struct bulk_exec *bx, void *arg)
{
    struct exec_ctx *x = arg;
    /* finish: RUN->CLEANUP
     */
    if (event_job_post_pack (x->sys->ctx->event,
                             x->job,
                             "finish",
                             0,
                             "{s:i}",
                             "status", x->status) < 0) {
        flux_log (x->sys->ctx->h,
                  LOG_ERR,
                  "sysjob %s: error posting finish event",
                  x->name);
    }
}

static void bulk_output_cb (struct bulk_exec *bx,
                            flux_subprocess_t *p,
                            const char *stream,
                            const char *data,
                            int data_len,
                            void *arg)
{
}

static void bulk_error_cb (struct bulk_exec *bx,
                           flux_subprocess_t *p,
                           void *arg)
{
}

int sysjob_start (struct sysjob *sys, struct job *job)
{
    struct exec_ctx *x = job_aux_get (job, "sysjob");

    if (!x)
        return -1;
    if (x->started)
        return 0; // already started (maintain idempotency)
    return bulk_exec_start (sys->ctx->h, x->bulk_exec);
}

/* N.B. see restart_map_cb() which does a similar thing for jobs read from
 * the KVS on job manager restart.
 */
int sysjob_create_finish (struct sysjob *sys, struct job *job)
{
    size_t index;
    json_t *entry;

    if (zhashx_insert (sys->ctx->active_jobs, &job->id, job) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (sys->ctx->max_jobid < job->id)
        sys->ctx->max_jobid = job->id;
    json_array_foreach (job->eventlog, index, entry) {
        const char *name;
        if (eventlog_entry_parse (entry, NULL, &name, NULL) < 0
            || journal_process_event (sys->ctx->journal,
                                      job->id,
                                      name,
                                      entry) < 0)
            return -1;
    }
    if (event_job_action (sys->ctx->event, job) < 0)
        return -1;
    /* The running jobs count is incremented in event_job_action() when a
     * job transitions into RUN state, but sysjobs (like restarting running
     * jobs) begin in RUN state, thus we must increment the count here.
     * This affects 'flux queue idle'
     */
    sys->ctx->running_jobs++;
    return 0;
}

static char *sysjob_create_eventlog (struct sysjob *sys,
                                     int urgency,
                                     int flags)
{
    json_t *eventlog;
    json_t *o;
    char *s;

    if (!(eventlog = json_array ()))
        goto nomem;
    // submit
    if (!(o = eventlog_entry_pack (0.,
                                   "submit",
                                   "{s:I s:i s:i s:i}",
                                   "userid", (json_int_t)sys->ctx->owner,
                                   "urgency", urgency,
                                   "flags", flags,
                                   "version", 1))
        || json_array_append_new (eventlog, o) < 0) {
        json_decref (o);
        goto nomem;
    }
    // validate
    if (!(o = eventlog_entry_create (0., "validate", NULL))
        || json_array_append_new (eventlog, o) < 0) {
        json_decref (o);
        goto nomem;
    }
    // depend
    if (!(o = eventlog_entry_create (0., "depend", NULL))
        || json_array_append_new (eventlog, o) < 0) {
        json_decref (o);
        goto nomem;
    }
    // priority
    if (!(o = eventlog_entry_pack (0., "priority", "{s:i}", "priority", 16))
        || json_array_append_new (eventlog, o) < 0) {
        json_decref (o);
        goto nomem;
    }
    // alloc
    if (!(o = eventlog_entry_create (0., "alloc", NULL))
        || json_array_append_new (eventlog, o) < 0) {
        json_decref (o);
        goto nomem;
    }
    if (!(s = eventlog_encode (eventlog)))
        goto nomem;
    json_decref (eventlog);
    return s;
nomem:
    errno = ENOMEM;
    ERRNO_SAFE_WRAP (json_decref, eventlog);
    return NULL;
}

static int nnodes_from_R (json_t *R)
{
    struct rlist *rl;
    int nnodes;

    if (!(rl = rlist_from_json (R, NULL)))
        return -1;
    nnodes = rlist_nnodes (rl);
    rlist_destroy (rl);
    return nnodes;
}

static char *sysjob_create_jobspec (struct sysjob *sys,
                                    const char *name,
                                    flux_cmd_t *cmd,
                                    json_t *R)
{
    int nnodes;
    char **argv = NULL;
    char **env = NULL;
    int argc;
    flux_jobspec1_t *js;
    char *s;

    if (!(nnodes = nnodes_from_R (R))
        || !(argv = cmd_argv_expand (cmd))
        || !(env = cmd_env_expand (cmd))) {
        ERRNO_SAFE_WRAP (free, argv);
        return NULL;
    }
    argc = flux_cmd_argc (cmd);
    if (!(js = flux_jobspec1_from_command (argc,
                                           argv,
                                           env,
                                           nnodes,  // ntasks (one per node)
                                           1,       // cores per task
                                           0,       // gpus per task
                                           nnodes,
                                           0.))     // duration
        || flux_jobspec1_attr_pack (js, "system.job.name", "s", name) < 0) {
        ERRNO_SAFE_WRAP (free, argv);
        ERRNO_SAFE_WRAP (free, env);
        flux_jobspec1_destroy (js);
        return NULL;
    }
    s = flux_jobspec1_encode (js, 0);
    free (argv);
    free (env);
    flux_jobspec1_destroy (js);
    return s;
}

static flux_future_t *sysjob_create_commit (struct sysjob *sys,
                                            struct job *job,
                                            const char *eventlog,
                                            const char *jobspec,
                                            const char *R)
{
    struct flux_kvs_txn *txn;
    char key[128];
    char *s = NULL;
    flux_future_t *f = NULL;

    if (!(txn = flux_kvs_txn_create ()))
        return NULL;
    if (flux_job_kvs_key (key, sizeof (key), job->id, "eventlog") < 0
        || flux_kvs_txn_put (txn, 0, key, eventlog) < 0)
        goto error;
    if (flux_job_kvs_key (key, sizeof (key), job->id, "R") < 0
        || flux_kvs_txn_put (txn, 0, key, R) < 0)
        goto error;

    if (flux_job_kvs_key (key, sizeof (key), job->id, "jobspec") < 0
        || !(s = sign_none_wrap (jobspec, strlen (jobspec), sys->ctx->owner))
        || flux_kvs_txn_put (txn, 0, key, s) < 0)
        goto error;
    if (!(f = flux_kvs_commit (sys->ctx->h, NULL, 0, txn)))
        goto error;
    if (flux_future_aux_set (f,
                             "job",
                             job_incref (job),
                             (flux_free_f)job_decref) < 0) {
        job_decref (job);
        goto error;
    }
    free (s);
    flux_kvs_txn_destroy (txn);
    return f;
error:
    flux_future_destroy (f);
    ERRNO_SAFE_WRAP (free, s);
    flux_kvs_txn_destroy (txn);
    return NULL;
}

/* Initialization of the fluid generator is deferred until the first sysjob
 * is created to ensure ctx->max_jobid is initialized.  That happens during
 * restart_from_kvs(), before the reactor starts, but after sysjob_ctx_create().
 */
static int sysjob_ctx_init (struct sysjob *sys)
{
    if (!sys->gen_initialized) {
        if (fluid_init (&sys->gen,
                        sysjob_fluid_generator_id,
                        fluid_get_timestamp (sys->ctx->max_jobid + 1)) < 0)
            return -1;
        sys->gen_initialized = true;
    }
    return 0;
}

flux_future_t *sysjob_create (struct sysjob *sys,
                              const char *name,
                              flux_cmd_t *cmd,
                              json_t *R_obj,
                              flux_error_t *error)
{
    flux_jobid_t id;
    char *eventlog;
    int flags = FLUX_JOB_SYSTEM;
    int urgency = FLUX_JOB_URGENCY_DEFAULT;
    char *jobspec = NULL;
    char *R = NULL;
    struct job *job = NULL;
    flux_future_t *f = NULL;
    struct exec_ctx *x;

    if (sysjob_ctx_init (sys) < 0
        || fluid_generate (&sys->gen, &id) < 0) {
        errprintf (error, "sysjob: error allocating job id");
        return NULL;
    }
    if (!(eventlog = sysjob_create_eventlog (sys, urgency, flags))) {
        errprintf (error,
                   "sysjob: error creating eventlog: %s",
                   strerror (errno));
        return NULL;
    }
    if (!(jobspec = sysjob_create_jobspec (sys, name, cmd, R_obj))
        || !(R = json_dumps (R_obj, JSON_COMPACT))) {
        errprintf (error, "sysjob: error encoding jobspec/R");
        errno = ENOMEM;
        goto done;
    }
    if (!(job = job_create_from_eventlog (id, eventlog, jobspec, R, error)))
        goto done;
    if (!(x = exec_ctx_create (sys, job, name, cmd, R_obj))
        || job_aux_set (job, "sysjob", x, (flux_free_f)exec_ctx_destroy) < 0) {
        errprintf (error, "sysjob: error creating exec context");
        exec_ctx_destroy (x);
        goto done;
    }
    if (!(f = sysjob_create_commit (sys, job, eventlog, jobspec, R))) {
        errprintf (error, "sysjob: error updating KVS");
        goto done;
    }
done:
    job_decref (job);
    ERRNO_SAFE_WRAP (free, R);
    ERRNO_SAFE_WRAP (free, jobspec);
    ERRNO_SAFE_WRAP (free, eventlog);
    return f;
}

int sysjob_create_get (flux_future_t *f, struct job **jobp)
{
    struct job *job;

    if (flux_rpc_get (f, NULL) < 0
        || !(job = flux_future_aux_get (f, "job")))
        return -1;
    if (jobp)
        *jobp = job;
    return 0;
}

void sysjob_ctx_destroy (struct sysjob *sys)
{
    if (sys) {
        int saved_errno = errno;
        free (sys);
        errno = saved_errno;
    }
}

struct sysjob *sysjob_ctx_create (struct job_manager *ctx)
{
    struct sysjob *sys;

    if (!(sys = calloc (1, sizeof (*sys))))
        return NULL;
    sys->ctx = ctx;
    if (flux_get_size (ctx->h, &sys->size) < 0)
        goto error;
    return sys;
error:
    sysjob_ctx_destroy (sys);
    return NULL;
}

static struct bulk_exec_ops bulk_exec_ops = {
    .on_start =     bulk_start_cb,
    .on_exit =      bulk_exit_cb,
    .on_complete =  bulk_complete_cb,
    .on_output =    bulk_output_cb,
    .on_error =     bulk_error_cb
};

// vi:ts=4 sw=4 expandtab
