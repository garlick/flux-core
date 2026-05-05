/************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* rhwloc_scheduling.c - build R scheduling key from hwloc topology */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <jansson.h>

#include "src/common/libutil/errprintf.h"
#include "src/common/libutil/errno_safe.h"
#include "rhwloc.h"

/* Internal: implemented in rhwloc_scheduling_treepool.c
 * Returns a complete scheduling object (format-specific structure).
 * Must set errno and errp on failure.
 */
json_t *rhwloc_scheduling_treepool (hwloc_topology_t topo,
                                    const char *ranks,
                                    flux_error_t *errp);

json_t *rhwloc_scheduling (hwloc_topology_t topo,
                           const char *format,
                           const char *ranks,
                           flux_error_t *errp)
{
    json_t *result = NULL;

    if (!topo || !format || !ranks) {
        errno = EINVAL;
        return NULL;
    }
    if (strcasecmp (format, "TreePool") == 0) {
        result = rhwloc_scheduling_treepool (topo, ranks, errp);
        /* Format function sets errno on failure */
    }
    else {
        errprintf (errp, "unknown scheduling format: %s", format);
        errno = EINVAL;
    }

    return result;
}

/* vi: ts=4 sw=4 expandtab
 */
