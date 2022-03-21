/************************************************************\
 * Copyright 2020 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _FLUX_JOB_LIST_JOB_STATS_H
#define _FLUX_JOB_LIST_JOB_STATS_H

#include <flux/core.h> /* FLUX_JOB_NR_STATES */

struct job_stats {
    unsigned int state_count[FLUX_JOB_NR_STATES];
    unsigned int failed;
    unsigned int timeout;
    unsigned int canceled;
};

/* Forward declaration of struct job to avoid circular header file
 *  dependemncy.
 */
struct job;
void job_stats_update (struct job_stats *stats,
                       struct job *job,
                       flux_job_state_t newstate);

json_t * job_stats_encode (struct job_stats *stats);

#endif /* ! _FLUX_JOB_LIST_JOB_STATS_H */

// vi: ts=4 sw=4 expandtab
