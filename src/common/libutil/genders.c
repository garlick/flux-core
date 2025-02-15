/************************************************************r
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
#include <errno.h>
#include <stdlib.h>
#include <argz.h>
#include <ctype.h>
#include <flux/core.h>

#include "src/common/libhostlist/hostlist.h"
#include "src/common/libczmqcontainers/czmq_containers.h"

#include "genders.h"
#include "strstrip.h"
#include "errprintf.h"
#include "errno_safe.h"

#define LINE_BUF_SIZE 4096

struct nodeattr {
    struct hostlist *nodes; // hostnames (genders LHS)
    int argc; // list of attr[=val] (genders RHS)
    char **argv;
};

struct genders {
    zlistx_t *nodeattrs; // list of 'struct nodeattr'
};

static void nodeattr_destroy (struct nodeattr *na)
{
    if (na) {
        int saved_errno = errno;
        hostlist_destroy (na->nodes);
        free (na->argv);
        free (na);
        errno = saved_errno;
    }
}

static void nodeattr_destructor (void **item)
{
    if (item) {
        nodeattr_destroy (*item);
        *item = NULL;
    }
}

/* helper for nodeattr_query()
 * Check if an attr[=val] expression matches an attribute name.
 * If val is non-NULL and the attribute matches, assign *val to the
 * attribute value or NULL if there isn't one.
 */
static bool match_attr (const char *attr,
                        const char *attrval,
                        const char **val)
{
    int keylen;
    char *delim;
    if ((delim = strchr (attrval, '=')))
        keylen = delim - attrval;
    else
        keylen = strlen (attrval);
    if (strlen (attr) != keylen || strncmp (attr, attrval, keylen) != 0)
        return false; // no match
    if (val)
        *val = delim ? delim + 1 : NULL;
    return true;
}

/* Does nodeattr contain attr?
 * If node is non-NULL, the nodeattr must contain the specified node.
 * This is just querying one 'struct nodeattr' and must be called from
 * genders->nodeattrs list iteration for a full genders query.
 */
static bool nodeattr_query (struct nodeattr *na,
                            const char *node,   // may be NULL
                            const char *attr,
                            const char **valp)  // may be NULL
{
    for (int i = 0; i < na->argc; i++) {
        const char *val = NULL;
        if (match_attr (attr, na->argv[i], &val)) {
            if (node && hostlist_find (na->nodes, node) == -1)
                break;
            if (valp)
                *valp = val;
            return true;
        }
    }
    return false;
}

/* Set (*argcp, *argvp) to an argv array derived from the specified
 * comma-separated attribute list.
 */
static int keyvals_parse (const char *attrs, int *argcp, char ***argvp)
{
    char *argz = NULL;
    size_t argz_len = 0;
    int argc;
    char **argv = NULL;

    if (argz_create_sep (attrs, ',', &argz, &argz_len) != 0) {
        errno = ENOMEM;
        goto error;
    }
    argc = argz_count (argz, argz_len);
    if (!(argv = calloc (argc + 1, sizeof (argv[0]))))
        goto error;
    argz_extract (argz, argz_len, argv);
    *argcp = argc;
    *argvp = argv;
    return 0;
error:
    ERRNO_SAFE_WRAP (free, argz);
    ERRNO_SAFE_WRAP (free, argv);
    return -1;
}

/* Create one 'struct nodeattr' given pre-split nodes and attrs from
 * a genders line.
 */
static struct nodeattr *nodeattr_create (const char *nodes,
                                         const char *attrs,
                                         int line_no,
                                         flux_error_t *error)
{
    struct nodeattr *na;

    if (!(na = calloc (1, sizeof (*na)))) {
        errprintf (error, "out of memory");
        return NULL;
    }
    if (!(na->nodes = hostlist_decode (nodes))) {
        errprintf (error,
                   "error decoding hostlist on line %d: %s",
                   line_no,
                   strerror (errno));
        goto error;
    }
    if (keyvals_parse (attrs, &na->argc, &na->argv) < 0) {
        errprintf (error,
                   "error parsing attributes on line %d: %s",
                   line_no,
                   strerror (errno));
        goto error;
    }
    return na;
error:
    nodeattr_destroy (na);
    return NULL;
}

/* Parse the genders file on 'f'.
 * The FILE is assumed to be rewound to the beginning.
 */
struct genders *genders_parse (FILE *f, flux_error_t *error)
{
    struct genders *g;
    char buf[LINE_BUF_SIZE];
    int line_no = 0;
    struct nodeattr *na;

    if (!(g = genders_create ()))
        return NULL;
    while (fgets (buf, sizeof (buf), f) != NULL) {
        char *key;
        char *val;

        line_no++;
        key = strstrip (buf);
        if (strlen (key) == 0)
            continue;
        val = key;
        while (*val && !isspace (*val))
            val++;
        *val++ = '\0';
        while (*val && isspace (*val))
            val++;
        if (val == key || strlen (val) == 0 || strlen (key) == 0) {
            errprintf (error, "parse error on line %d", line_no);
            errno = EINVAL;
            goto error;
        }
        if (!(na = nodeattr_create (key, val, line_no, error))) {
            errno = EINVAL;
            goto error;
        }
        if (!zlistx_add_end (g->nodeattrs, na)) {
            nodeattr_destroy (na);
            goto nomem;
        }
    }
    return g;
nomem:
    errprintf (error, "out of memory");
    errno = ENOMEM;
error:
    genders_destroy (g);
    return NULL;
}

static char *attr_value_subst (const char *node, const char *val)
{
    const char *tok;
    char *newval;

    if ((tok = strstr (val, "%n"))) {
        int len1 = tok - val; // prefix
        int len2 = strlen (node); // token substitution
        int len3 = strlen (val) - len1 - 2; // suffix
        if ((newval = calloc (1, len1 + len2 + len3 + 1))) {
            char *cp = newval;
            for (int i = 0; i < len1; i++)
                *cp++ = val[i];
            for (int i = 0; i < len2; i++)
                *cp++ = node[i];
            for (int i = 0; i < len3; i++)
                *cp++ = val[len1 + 2 + i];
        }
    }
    else
        newval = strdup (val);
    return newval;
}

/* Classic query #1: Does node have attr?
 * Enhancement: if yes, set *value to its attr value (with %n substitution)
 */
bool genders_node_attr (struct genders *g,
                        const char *node,
                        const char *attr,
                        char **value)
{
    struct nodeattr *na;
    const char *val;

    na = zlistx_first (g->nodeattrs);
    while (na) {
        if (nodeattr_query (na, node, attr, value ? &val : NULL)) {
            if (value)
                *value = attr_value_subst (node, val);
            return true;
        }
        na = zlistx_next (g->nodeattrs);
    }
    return false;
}

/* Classic query #2: Which nodes have attr?
 */
struct hostlist *genders_attr_nodes (struct genders *g, const char *attr)
{
    struct nodeattr *na;
    struct hostlist *hl;

    if (!(hl = hostlist_create ()))
        return NULL;
    na = zlistx_first (g->nodeattrs);
    while (na) {
        if (nodeattr_query (na, NULL, attr, NULL))
            if (hostlist_append_list (hl, na->nodes) < 0)
                goto error;
        na = zlistx_next (g->nodeattrs);
    }
    hostlist_sort (hl);
    hostlist_uniq (hl);
    return hl;
error:
    hostlist_destroy (hl);
    return NULL;
}

/* Classic query #3: Which attrs have node?
 * We'll dump them into an argv array.
 */
int genders_node_attrs (struct genders *g, const char *node, char ***argvp)
{
    struct nodeattr *na;
    char *argz = NULL;
    size_t argz_len = 0;
    char **argv;
    int argc;

    na = zlistx_first (g->nodeattrs);
    while (na) {
        if (hostlist_find (na->nodes, node) != -1) {
            for (int i = 0; i < na->argc; i++) {
                int e;
                char *cp;
                if ((cp = strchr (na->argv[i], '='))) { // needs a trim
                    char *cpy = strdup (na->argv[i]);
                    if (!cpy)
                        goto error;
                    cpy[cp - na->argv[i]] = '\0';
                    e = argz_add (&argz, &argz_len, cpy);
                    free (cpy);
                }
                else
                    e = argz_add (&argz, &argz_len, na->argv[i]);
                if (e != 0) {
                    errno = e;
                    goto error;
                }
            }
        }
        na = zlistx_next (g->nodeattrs);
    }
    argc = argz_count (argz, argz_len);
    if (!(argv = calloc (argc + 1, sizeof (argv[0]))))
        goto error;
    argz_extract (argz, argz_len, argv);
    *argvp = argv;
    return argc;
error:
    ERRNO_SAFE_WRAP (free, argz);
    return -1;
}

void genders_destroy (struct genders *g)
{
    if (g) {
        int saved_errno = errno;
        zlistx_destroy (&g->nodeattrs);
        errno = saved_errno;
    }
}

struct genders *genders_create (void)
{
    struct genders *g;

    if (!(g = calloc (1, sizeof (*g))))
        return NULL;
    if (!(g->nodeattrs = zlistx_new ())) {
        errno = ENOMEM;
        goto error;
    }
    return g;
    zlistx_set_destructor (g->nodeattrs, nodeattr_destructor);
error:
    genders_destroy (g);
    return NULL;
}


// vi:ts=4 sw=4 expandtab
