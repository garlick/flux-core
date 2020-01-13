/************************************************************\
 * Copyright 2019 Lawrence Livermore National Security, LLC
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
#include <sys/param.h>
#include <unistd.h>
#include <jansson.h>
#include <flux/core.h>

#include "src/common/libtap/tap.h"
#include "src/broker/boot_config.h"


static void
create_test_file (const char *dir, char *prefix, char *path, size_t pathlen,
                  const char *contents)
{
    int fd;
    if (snprintf (path,
                  pathlen,
                  "%s/%s.XXXXXX.toml",
                  dir ? dir : "/tmp",
                  prefix) >= pathlen)
        BAIL_OUT ("snprintf overflow");
    fd = mkstemps (path, 5);
    if (fd < 0)
        BAIL_OUT ("mkstemp %s: %s", path, strerror (errno));
    if (write (fd, contents, strlen (contents)) != strlen (contents))
        BAIL_OUT ("write %s: %s", path, strerror (errno));
    if (close (fd) < 0)
        BAIL_OUT ("close %s: %s", path, strerror (errno));
}

static void
create_test_dir (char *dir, int dirlen)
{
    const char *tmpdir = getenv ("TMPDIR");
    if (snprintf (dir,
                  dirlen,
                  "%s/cf.XXXXXXX",
                  tmpdir ? tmpdir : "/tmp") >= dirlen)
        BAIL_OUT ("snprintf overflow");
    if (!mkdtemp (dir))
        BAIL_OUT ("mkdtemp %s: %s", dir, strerror (errno));
    setenv ("FLUX_CONF_DIR", dir, 1);
}

void test_parse (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    json_t *hosts = NULL;
    struct boot_conf conf;
    uint32_t rank;
    char uri[MAX_URI + 1];
    int rc;
    const char *input = \
"[bootstrap]\n" \
"default_port = 42\n" \
"default_bind = \"tcp://en0:%p\"\n" \
"default_connect = \"tcp://x%h:%p\"\n" \
"hosts = [\n" \
"  { host = \"foo0\" },\n" \
"  { host = \"foo[1-62]\" },\n" \
"  { host = \"foo63\" },\n" \
"]\n";

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);
    rc = boot_config_parse (h, &conf, &hosts);
    ok (rc == 0,
        "boot_conf_parse worked");
    ok (hosts != NULL && json_array_size (hosts) == 64,
        "got 64 hosts");
    if (hosts == NULL)
        BAIL_OUT ("cannot continue without hosts array");

    ok (conf.default_port == 42,
        "set default_port correctly");
    ok (!strcmp (conf.default_bind, "tcp://en0:42"),
        "and set default_bind correctly (with %%p substitution)");
    ok (!strcmp (conf.default_connect, "tcp://x%h:42"),
        "and set default_connect correctly (with %%p substitution)");

    ok (boot_config_getrankbyname (hosts, "foo0", &rank) == 0
        && rank == 0,
       "boot_config_getrankbyname found rank 0");
    ok (boot_config_getrankbyname (hosts, "foo1", &rank) == 0
        && rank == 1,
       "boot_config_getrankbyname found rank 1");
    ok (boot_config_getrankbyname (hosts, "foo42", &rank) == 0
        && rank == 42,
       "boot_config_getrankbyname found rank 42");
    ok (boot_config_getrankbyname (hosts, "notfound", &rank) < 0,
       "boot_config_getrankbyname fails on unknown entry");

    ok (boot_config_getbindbyrank (hosts, &conf, 0, uri, sizeof (uri)) == 0
        && !strcmp (uri, "tcp://en0:42"),
        "boot_config_getbindbyrank 0 works with expected value");
    ok (boot_config_getbindbyrank (hosts, &conf, 1, uri, sizeof (uri)) == 0
        && !strcmp (uri, "tcp://en0:42"),
        "boot_config_getbindbyrank 1 works with expected value");
    ok (boot_config_getbindbyrank (hosts, &conf, 63, uri, sizeof (uri)) == 0
        && !strcmp (uri, "tcp://en0:42"),
        "boot_config_getbindbyrank 63 works with expected value");
    ok (boot_config_getbindbyrank (hosts, &conf, 64, uri, sizeof (uri))  < 0,
        "boot_config_getbindbyrank 64 fails");

    ok (boot_config_geturibyrank (hosts, &conf, 0, uri, sizeof (uri)) == 0
        && !strcmp (uri, "tcp://xfoo0:42"),
        "boot_config_geturibyrank 0 works with expected value");
    ok (boot_config_geturibyrank (hosts, &conf, 1, uri, sizeof (uri)) == 0
        && !strcmp (uri, "tcp://xfoo1:42"),
        "boot_config_geturibyrank 1 works with expected value");
    ok (boot_config_geturibyrank (hosts, &conf, 63, uri, sizeof (uri)) == 0
        && !strcmp (uri, "tcp://xfoo63:42"),
        "boot_config_geturibyrank 63 works with expected value");
    ok (boot_config_geturibyrank (hosts, &conf, 64, uri, sizeof (uri))  < 0,
        "boot_config_geturibyrank 64 fails");

    json_decref (hosts);

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_overflow_bind (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    struct boot_conf conf;
    char t[MAX_URI*2];
    json_t *hosts;

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    if (snprintf (t,
                  sizeof (t),
                  "[bootstrap]\ndefault_bind=\"%*s\"\nhosts=[\"foo\"]\n",
                  MAX_URI+2, "foo") >= sizeof (t))
        BAIL_OUT ("snprintf overflow");
    create_test_file (dir, "boot", path, sizeof (path), t);

    ok (boot_config_parse (h, &conf, &hosts) == -1,
        "boot_conf_parse caught default_bind overflow");

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_overflow_connect (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    struct boot_conf conf;
    char t[MAX_URI*2];
    json_t *hosts;

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    if (snprintf (t,
                  sizeof (t),
                  "[bootstrap]\ndefault_connect=\"%*s\"\nhosts=[\"foo\"]\n",
                  MAX_URI+2, "foo") >= sizeof (t))
        BAIL_OUT ("snprintf overflow");
    create_test_file (dir, "boot", path, sizeof (path), t);

    ok (boot_config_parse (h, &conf, &hosts) == -1,
        "boot_conf_parse caught default_connect overflow");

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_bad_hosts_entry (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    struct boot_conf conf;
    json_t *hosts;
    const char *input = \
"[bootstrap]\n" \
"hosts = [\n" \
"  42,\n" \
"]\n";

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);

    ok (boot_config_parse (h, &conf, &hosts) == -1,
        "boot_config_parse failed bad hosts entry");

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_missing_info (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    json_t *hosts;
    struct boot_conf conf;
    char uri[MAX_URI + 1];
    uint32_t rank;
    const char *input = \
"[bootstrap]\n" \
"hosts = [\n" \
"  { host = \"foo\" },\n" \
"]\n";

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);

    if (boot_config_parse (h, &conf, &hosts) < 0)
        BAIL_OUT ("boot_config_parse unexpectedly failed");
    if (!hosts)
        BAIL_OUT ("cannot continue without hosts array");
    ok (boot_config_getrankbyname (hosts, "foo", &rank) == 0
        && rank == 0,
        "boot_config_getrankbyname found entry");
    ok (boot_config_getbindbyrank (hosts, &conf, 0, uri, sizeof(uri)) < 0,
        "boot_config_getbindbyrank fails due to missing bind uri");
    ok (boot_config_geturibyrank (hosts, &conf, 0, uri, sizeof(uri)) < 0,
        "boot_config_geturibyrank fails due to missing connect uri");

    json_decref (hosts);

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_bad_host_idset (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    struct boot_conf conf;
    json_t *hosts;
    const char *input = \
"[bootstrap]\n" \
"hosts = [\n" \
"  { host=\"foo[1-]\" },\n" \
"]\n";

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);

    ok (boot_config_parse (h, &conf, &hosts) == -1,
        "boot_config_parse failed on host entry containing bad idset");

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_bad_host_bind (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    struct boot_conf conf;
    json_t *hosts;
    char uri[MAX_URI + 1];
    const char *input = \
"[bootstrap]\n" \
"hosts = [\n" \
"  { host=\"foo\", bind=42 },\n" \
"]\n";

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);

    /* hosts will initially parse OK then fail in getbindbyrank */
    if (boot_config_parse (h, &conf, &hosts) < 0)
        BAIL_OUT ("boot_config_parse unexpectedly failed");
    ok (boot_config_getbindbyrank (hosts, &conf, 0, uri, sizeof (uri)) < 0,
        "boot_config_getbindbyrank failed on hoste entry wtih wrong bind type");

    json_decref (hosts);

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}


/* Just double check that an array with mismatched types
 * fails early with the expected libtomlc99 error.
 */
void test_toml_mixed_array (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    const flux_conf_t *conf;
    flux_conf_error_t error;
    const char *input = \
"[bootstrap]\n" \
"hosts = [\n" \
"  \"bar\",\n" \
"  { host = \"foo\" },\n" \
"]\n";

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);

    conf = flux_get_conf (h, &error);
    ok (conf == NULL && (strstr (error.errbuf, "array type mismatch")
        || strstr (error.errbuf, "string array can only contain strings")),
        "Mixed type hosts array fails with reasonable error");
    diag ("%s: line %d: %s", error.filename, error.lineno, error.errbuf);

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_no_hosts (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    json_t *hosts;
    struct boot_conf conf;
    const char *input = \
"[bootstrap]\n";

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);

    hosts = (json_t *)(uintptr_t)1;
    ok (boot_config_parse (h, &conf, &hosts) == 0 && hosts == NULL,
        "boot_config_parse works with missing hosts array");

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_empty_hosts (const char *dir)
{
    char path[PATH_MAX + 1];
    flux_t *h;
    json_t *hosts;
    struct boot_conf conf;
    const char *input = \
"[bootstrap]\n" \
"hosts = [\n" \
"]\n";
;

    if (!(h = flux_open ("loop://", 0)))
        BAIL_OUT ("can't continue without loop handle");

    create_test_file (dir, "boot", path, sizeof (path), input);

    hosts = (json_t *)(uintptr_t)1;
    ok (boot_config_parse (h, &conf, &hosts) == 0 && hosts == NULL,
        "boot_config_parse works with empty hosts array");

    if (unlink (path) < 0)
        BAIL_OUT ("could not cleanup test file %s", path);

    flux_close (h);
}

void test_format (void)
{
    char buf[MAX_URI + 1];

    ok (boot_config_format_uri (buf, sizeof (buf), "abcd", NULL, 0) == 0
        && !strcmp (buf, "abcd"),
        "format: plain string copy works");
    ok (boot_config_format_uri (buf, sizeof (buf), "abcd:%p", NULL, 42) == 0
        && !strcmp (buf, "abcd:42"),
        "format: %%p substitution works end string");
    ok (boot_config_format_uri (buf, sizeof (buf), "a%pb", NULL, 42) == 0
        && !strcmp (buf, "a42b"),
        "format: %%p substitution works mid string");
    ok (boot_config_format_uri (buf, sizeof (buf), "%p:abcd", NULL, 42) == 0
        && !strcmp (buf, "42:abcd"),
        "format: %%p substitution works begin string");
    ok (boot_config_format_uri (buf, sizeof (buf), "%h", NULL, 0) == 0
        && !strcmp (buf, "%h"),
        "format: %%h passes through when host=NULL");
    ok (boot_config_format_uri (buf, sizeof (buf), "%h", "foo", 0) == 0
        && !strcmp (buf, "foo"),
        "format: %%h substitution works");
    ok (boot_config_format_uri (buf, sizeof (buf), "%%", NULL, 0) == 0
        && !strcmp (buf, "%"),
        "format: %%%% literal works");
    ok (boot_config_format_uri (buf, sizeof (buf), "a%X", NULL, 0) == 0
        && !strcmp (buf, "a%X"),
        "format: unknown token passes through");

    ok (boot_config_format_uri (buf, 5, "abcd", NULL, 0) == 0
        && !strcmp (buf, "abcd"),
        "format: copy abcd to buf[5] works");
    ok (boot_config_format_uri (buf, 4, "abcd", NULL, 0) < 0,
        "format: copy abcd to buf[4] fails");

    ok (boot_config_format_uri (buf, 5, "a%p", NULL, 123) == 0
        && !strcmp (buf, "a123"),
        "format: %%p substitution into exact size buf works");
    ok (boot_config_format_uri (buf, 4, "a%p", NULL, 123) < 0,
        "format: %%p substitution overflow detected");

    ok (boot_config_format_uri (buf, 5, "a%h", "abc", 0) == 0
        && !strcmp (buf, "aabc"),
        "format: %%h substitution into exact size buf works");
    ok (boot_config_format_uri (buf, 4, "a%h", "abc", 0) < 0,
        "format: %%h substitution overflow detected");
}

int main (int argc, char **argv)
{
    char dir[PATH_MAX + 1];

    plan (NO_PLAN);

    test_format ();

    create_test_dir (dir, sizeof (dir));

    test_parse (dir);
    test_overflow_bind (dir);
    test_overflow_connect (dir);
    test_bad_hosts_entry (dir);
    test_bad_host_idset (dir);
    test_bad_host_bind (dir);
    test_no_hosts (dir);
    test_empty_hosts (dir);
    test_missing_info (dir);
    test_toml_mixed_array (dir);

    if (rmdir (dir) < 0)
        BAIL_OUT ("could not cleanup test dir %s", dir);

    done_testing ();
    return 0;
}

/*
 * vi:ts=4 sw=4 expandtab
 */