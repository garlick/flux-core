/************************************************************\
 * Copyright 2018 Lawrence Livermore National Security, LLC
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
#include <flux/core.h>

#include "job.h"

struct state {
    flux_job_state_t state;
    const char *long_upper;
    const char *long_lower;
    const char *short_upper;
    const char *short_lower;
};

static struct state states[] = {
    { FLUX_JOB_STATE_NEW,       "NEW",      "new",      "N", "n" },
    { FLUX_JOB_STATE_DEPEND,    "DEPEND",   "depend",   "D", "d" },
    { FLUX_JOB_STATE_PRIORITY,  "PRIORITY", "priority", "P", "p" },
    { FLUX_JOB_STATE_SCHED,     "SCHED",    "sched",    "S", "s" },
    { FLUX_JOB_STATE_RUN,       "RUN",      "run",      "R", "r" },
    { FLUX_JOB_STATE_CLEANUP,   "CLEANUP",  "cleanup",  "C", "c" },
    { FLUX_JOB_STATE_INACTIVE,  "INACTIVE", "inactive", "I", "i" },
};
static const size_t states_count = sizeof (states) / sizeof (states[0]);

static const char *format_state (struct state *state, const char *fmt)
{
    switch (*fmt) {
        case 'c':
            return state->short_lower;
        case 'C':
            return state->short_upper;
        case 's':
            return state->long_lower;
        case 'S':
        default:
            return state->long_upper;
    }
}

const char *flux_job_statetostr (flux_job_state_t state, const char *fmt)
{
    struct state unknown = { 0, "(unknown)", "(unknown)", "?", "?" };

    for (int i = 0; i < states_count; i++)
        if (states[i].state == state)
            return format_state (&states[i], fmt);
    return format_state (&unknown, fmt);
}

int flux_job_strtostate (const char *s, flux_job_state_t *state)
{
    if (s && state) {
        for (int i = 0; i < states_count; i++) {
            if (!strcmp (states[i].short_lower, s)
                || !strcmp (states[i].short_upper, s)
                || !strcmp (states[i].long_lower, s)
                || !strcmp (states[i].long_upper, s)) {
                *state = states[i].state;
                return 0;
            }
        }
    }
    errno = EINVAL;
    return -1;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
