/************************************************************\
 * Copyright 2025 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _LIBSUBPROCESS_CHILD_WATCHER_H
#define _LIBSUBPROCESS_CHILD_WATCHER_H

#include <flux/core.h>

flux_watcher_t *child_watcher_create (flux_reactor_t *r,
                                      int pid,
                                      flux_watcher_f cb,
                                      void *arg);
int child_watcher_get_rpid (flux_watcher_t *w);
int child_watcher_get_rstatus (flux_watcher_t *w);

#endif /* !_LIBSUBPROCESS_CHILD_WATCHER_H */

// vi:ts=4 sw=4 expandtab
