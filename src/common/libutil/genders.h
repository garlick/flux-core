/************************************************************\
 * Copyright 2025 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _UTIL_GENDERS_H
#define _UTIL_GENDERS_H

#include <stdio.h>
#include <flux/core.h>
#include <flux/hostlist.h>

struct genders *genders_create (void);
struct genders *genders_parse (FILE *f, flux_error_t *error);
void genders_destroy (struct genders *g);

/* If node has the specified attribute, return true and either:
 * - set *value to the attribute's value if it has one (caller must free)
 * - set *value to NULL if the attribute doesn't have a value.
 * If node doesn't have the specified attribute, return false and leave *value
 * alone.
 */
bool genders_node_attr (struct genders *g,
                        const char *node,
                        const char *attr,
                        char **value);

/* Build a hostlist of all nodes that have the specified attribute.
 * The caller must release it with hostlist_destroy().
 */
struct hostlist *genders_attr_nodes (struct genders *g, const char *attr);

/* Build an argv array containing all the attributes (without values)
 * assigned to node.  The caller must free (*argvp).
 * The returned value is argc, or -1 on error.
 */
int genders_node_attrs (struct genders *g, const char *node, char ***argvp);

#endif

// vi:ts=4 sw=4 expandtab
