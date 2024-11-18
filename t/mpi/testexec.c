/************************************************************\
 * Copyright 2021 Lawrence Livermore National Security, LLC
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

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>

#include "mpi.h"

extern char ** environ;

void die (const char *fmt, ...)
{
    va_list ap;
    char buf[128];

    va_start (ap, fmt);
    vsnprintf (buf, sizeof (buf), fmt, ap);
    va_end (ap);
    fprintf (stderr, "testexec: %s\n", buf);
    exit (1);
}

int main(int argc, char *argv[])
{
    int exit_rc = 0;

    if (argc != 2)
        die ("Usage: mpirun testexec prog");
    MPI_Init (&argc, &argv);
    pid_t pid = fork();
    if (pid < 0)
        die ("fork error");
    if (pid == 0) {
        errno = 0;
        execve (argv[1], &argv[1], environ);
        die ("exec %s: %s", argv[1], strerror (errno));
    }

    printf ("waiting for %d\n", pid);
	fflush (stdout);
	int status;
    if (waitpid (pid, &status, 0) < 0)
	    die ("waitpid %d: %s", pid, strerror (errno));
    fprintf (stderr, "wait status=%d\n", status);
    if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
        exit_rc = 1;

    MPI_Finalize();
    return exit_rc;
}

// vi:ts=4 sw=4 expandtab
