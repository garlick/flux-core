/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _BROKER_MODULE_BUILTIN_H
#define _BROKER_MODULE_BUILTIN_H

#include <flux/core.h>
#include "broker.h"
#include "module_builtin.h"

struct module_builtin {
    const char *name;
    mod_main_f *main;
};

/* Load all the builtin broker modules.
 */
int module_builtin_load_all (struct broker *ctx, flux_error_t *error);

#endif /* !_BROKER_MODULE_BUILTIN_H */

// vi:ts=4 sw=4 expandtab
