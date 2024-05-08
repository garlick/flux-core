/************************************************************\
 * Copyright 2024 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _FLUX_JOB_MANAGER_SYSJOB_H
#define _FLUX_JOB_MANAGER_SYSJOB_H

#include <flux/core.h>
#include <jansson.h>

#include "job-manager.h"
#include "job.h"

struct sysjob *sysjob_ctx_create (struct job_manager *ctx);
void sysjob_ctx_destroy (struct sysjob *sys);

/* Run cmd across the ranks of R (one task per node).
 * This creates the job in RUN state and commits artifacts to the KVS.
 * The future is fulfilled once the KVS commit completes.
 * Once that happens, the 'struct job' can be obtained with sysjob_create_get()
 * and made known to the job manager via sysjob_create_finish().
 */
flux_future_t *sysjob_create (struct sysjob *sys,
                              const char *name,
                              flux_cmd_t *cmd,
                              json_t *R,
                              flux_error_t *error);

int sysjob_create_get (flux_future_t *f, struct job **job);

/* Insert the job into the active_jobs hash and process its events, which
 * triggers sysjob_start().  The job's events are posted to the journal
 * at this point, making the job visible to flux-jobs(1).
 */
int sysjob_create_finish (struct sysjob *sys, struct job *job);

/* Begin executing job.
 * This is called from event_job_action() in RUN state, thus it is
 * triggered by calling sysjob_create_finish().
 * N.B. idempotent
 */
int sysjob_start (struct sysjob *sys, struct job *job);

#endif /* ! _FLUX_JOB_MANAGER_SYSJOB_H */

// vi:ts=4 sw=4 expandtab
