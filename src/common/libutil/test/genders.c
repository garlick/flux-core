/************************************************************\
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
#include <stdio.h>
#include <flux/core.h>

#include "ccan/str/str.h"
#include "genders.h"
#include "log.h"

int cmd_hasattr (struct genders *g, const char *node, const char *attr)
{
    if (genders_node_attr (g, node, attr, NULL))
        return 0;
    return 1; // node doesn't have attr
}

int cmd_value (struct genders *g, const char *node, const char *attr)
{
    char *val;
    if (genders_node_attr (g, node, attr, &val)) {
        if (val)
            printf ("%s\n", val);
        free (val);
        return 0;
    }
    return 1; // node doesn't have attr
}

int cmd_nodes (struct genders *g, const char *attr)
{
    struct hostlist *hl;
    char *s;

    if (!(hl = genders_attr_nodes (g, attr))
        || !(s = hostlist_encode (hl))) {
        log_err ("genders query error");
        hostlist_destroy (hl);
        return 1; // error
    }
    printf ("%s\n", s);
    free (s);
    hostlist_destroy (hl);
    return 0;
}

int cmd_attrs (struct genders *g, const char *node)
{
    int argc;
    char **argv;

    argc = genders_node_attrs (g, node, &argv);
    if (argc < 0)
        return 1; // error
    for (int i = 0; i < argc; i++)
        printf ("%s\n", argv[i]);
    free (argv);
    return 0;
}

void usage (void)
{
    printf ("Usage: test_genders PATH hasattr node attr\n"
            "       test_genders PATH value node attr\n"
            "       test_genders PATH nodes attr\n"
            "       test_genders PATH attrs node\n");
    exit (1);
}


int main(int argc, char** argv)
{
    FILE *f;
    flux_error_t error;
    struct genders *g;
    const char *path;
    const char *cmd;
    int index = 1;
    int exit_rc = 1;

    log_init ("genders");

    if (argc < 3)
        usage ();
    path = argv[index++];
    cmd = argv[index++];

    if (!(f = fopen (path, "r")))
        log_err_exit ("%s", path);
    if (!(g = genders_parse (f, &error)))
        log_msg_exit ("%s", error.text);
    fclose (f);

    if (streq (cmd, "hasattr") && argc == 5) {
        const char *node = argv[index++];
        const char *attr = argv[index++];
        exit_rc = cmd_hasattr (g, node, attr);
    }
    else if (streq (cmd, "value") && argc == 5) {
        const char *node = argv[index++];
        const char *attr = argv[index++];
        exit_rc = cmd_value (g, node, attr);
    }
    else if (streq (cmd, "nodes") && argc == 4) {
        const char *attr = argv[index++];
        exit_rc = cmd_nodes (g, attr);
    }
    else if (streq (cmd, "attrs") && argc == 4) {
        const char *node = argv[index++];
        exit_rc = cmd_attrs (g, node);
    }
    else
        usage ();

    genders_destroy (g);
    return exit_rc;
}

// vi:ts=4 sw=4 expandtab
