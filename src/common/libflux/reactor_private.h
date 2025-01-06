/************************************************************\
 * Copyright 2021 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _FLUX_CORE_REACTOR_PRIVATE_H
#define _FLUX_CORE_REACTOR_PRIVATE_H

#include "reactor.h"

/* retrieve underlying loop implementation - for watcher_wrap.c only */
void *reactor_get_loop (flux_reactor_t *r);

typedef void (*sigchld_f)(pid_t pid, int status, void *arg);

int reactor_sigchld_register (flux_reactor_t *r, sigchld_f cb, void *arg);
void reactor_sigchld_unregister (flux_reactor_t *r);

int reactor_get_flags (flux_reactor_t *r);

#endif /* !_FLUX_CORE_REACTOR_PRIVATE_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
