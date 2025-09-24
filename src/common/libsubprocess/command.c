/************************************************************\
 * Copyright 2018 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* command.c - accessors for RFC 42 command object */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <sys/types.h>
#ifdef HAVE_ARGZ_ADD
#include <argz.h>
#else
#include "src/common/libmissing/argz.h"
#endif
#ifdef HAVE_ENVZ_ADD
#include <envz.h>
#else
#include "src/common/libmissing/envz.h"
#endif
#include <fnmatch.h>
#include <jansson.h>

#include "src/common/libutil/errno_safe.h"
#include "ccan/str/str.h"

#include "command_private.h"
#include "command.h"

struct flux_command {
    json_t *obj;
};

static bool argcheck_ptr_is_set (const void *arg)
{
    if (!arg) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static bool argcheck_int_is_bounded (int i, int min, int max)
{
    if (i < min || i > max) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static bool argcheck_cmd (const flux_cmd_t *cmd)
{
    if (!cmd || !cmd->obj) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static bool jerror (json_error_t *error, const char *msg)
{
    if (error)
        snprintf (error->text, sizeof (error->text), "%s", msg);
    errno = EINVAL;
    return false;
}

// see RFC 42
static bool argcheck_cmd_json (const flux_cmd_t *cmd, json_error_t *error)
{
    const char *cwd = NULL;
    json_t *cmdline;
    json_t *env;
    json_t *opts;
    json_t *channels;
    json_t *msgchans = NULL;

    if (!argcheck_cmd (cmd))
        return false;
    if (json_unpack_ex (cmd->obj,
                        error,
                        0,
                        "{s?s s:o s:o s:o s:o s?o !}",
                        "cwd", &cwd,
                        "cmdline", &cmdline,
                        "env", &env,
                        "opts", &opts,
                        "channels", &channels,
                        "msgchans", &msgchans) < 0) {
        errno = EINVAL;
        return false;
    }
    if (!json_is_array (cmdline))
        return jerror (error, "cmdline is not an array");
    if (json_array_size (cmdline) == 0)
        return jerror (error, "cmdline array is empty");
    if (!json_is_object (env))
        return jerror (error, "env is not an object");
    if (!json_is_object (opts))
        return jerror (error, "opts is not an object");
    if (!json_is_array (channels))
        return jerror (error, "channels is not an array");
    if (msgchans && !json_is_array (msgchans))
        return jerror (error, "msgchans is not an array");
    return true;
}

static json_t *cmd_get_required_array (const flux_cmd_t *cmd, const char *name)
{
    json_t *val;
    if (!argcheck_cmd (cmd))
        return NULL;
    if (json_unpack (cmd->obj, "{s:o}", name, &val) < 0
        || !json_is_array (val)) {
        errno = EINVAL;
        return NULL;
    }
    return val;
}

static json_t *cmd_get_required_object (const flux_cmd_t *cmd, const char *name)
{
    json_t *val;
    if (!argcheck_cmd (cmd))
        return NULL;
    if (json_unpack (cmd->obj, "{s:o}", name, &val) < 0
        || !json_is_object (val)) {
        errno = EINVAL;
        return NULL;
    }
    return val;
}

void flux_cmd_destroy (flux_cmd_t *cmd)
{
    if (cmd) {
        int saved_errno = errno;
        json_decref (cmd->obj);
        free (cmd);
        errno = saved_errno;
    }
}

static int cmdline_to_argz (json_t *cmdline, char **argzp, size_t *argz_lenp)
{
    size_t index;
    json_t *entry;
    size_t argz_len = 0;
    char *argz = NULL;

    json_array_foreach (cmdline, index, entry) {
        const char *val;
        int e;
        if (!(val = json_string_value (entry))) {
            errno = EINVAL;
            goto error;
        }
        if ((e = argz_add (&argz, &argz_len, val)) != 0) {
            errno = e;
            goto error;
        }
    };
    *argzp = argz;
    *argz_lenp = argz_len;
    return 0;
error:
    ERRNO_SAFE_WRAP (free, argz);
    return -1;
}

static int env_to_envz (json_t *env, char **envzp, size_t *envz_lenp)
{
    const char *key;
    json_t *value;
    size_t envz_len = 0;
    char *envz = NULL;

    json_object_foreach (env, key, value) {
        const char *val;
        int e;
        if (!(val = json_string_value (value))) {
            errno = EINVAL;
            goto error;
        }
        if ((e = envz_add (&envz, &envz_len, key, val)) != 0) {
            errno = e;
            goto error;
        }
    };
    *envzp = envz;
    *envz_lenp = envz_len;
    return 0;
error:
    ERRNO_SAFE_WRAP (free, envz);
    return -1;
}

/* Realloc argz to fit argv array at the front, so the whole thing can be
 * freed in one go.  On success, the old argz pointer should no longer be used.
 */
static char **extract_argz_monofree (char *argz, size_t argz_len)
{
    char **argv;
    size_t argc = argz_count (argz, argz_len);
    size_t argv_size = sizeof (argv[0]) * (argc + 1);

    if (!(argv = realloc (argz, argv_size + argz_len)))
        return NULL;
    argz = (char *)argv + argv_size;
    memmove (argz, argv, argz_len);
    argz_extract (argz, argz_len, argv);
    argv[argc] = NULL;

    return argv;
}

static char **cmdline_decode (json_t *cmdline)
{
    size_t argz_len;
    char *argz;
    char **argv;

    if (cmdline_to_argz (cmdline, &argz, &argz_len) < 0)
        return NULL;
    if (!(argv = extract_argz_monofree (argz, argz_len))) {
        ERRNO_SAFE_WRAP (free, argz);
        return NULL;
    }
    return argv;
}

static json_t *cmdline_encode (int argc, char **argv)
{
    json_t *a;

    if (argc > 0 && !argv) {
        errno = EINVAL;
        return NULL;
    }
    if (!(a = json_array ()))
        goto nomem;
    for (int i = 0; i < argc; i++) {
        json_t *o;
        if (!(o = json_string (argv[i])) || json_array_append_new (a, o) < 0) {
            json_decref (o);
            goto nomem;
        }
    }
    return a;
nomem:
    json_decref (a);
    errno = ENOMEM;
    return NULL;
}

static char **env_decode (json_t *env)
{
    size_t envz_len;
    char *envz;
    char **envrn;

    if (env_to_envz (env, &envz, &envz_len) < 0)
        return NULL;
    if (!(envrn = extract_argz_monofree (envz, envz_len))) {
        ERRNO_SAFE_WRAP (free, envz);
        return NULL;
    }
    return envrn;
}

static json_t *env_encode (char **env)
{
    json_t *obj;
    if (!(obj = json_object ()))
        goto nomem;
    if (env) {
        for (int i = 0; env[i] != NULL; i++) {
            char *cp;
            json_t *val = NULL;
            char *key;
            if (!(cp = strrchr (env[i], '='))) {
                errno = EINVAL;
                goto error;
            }
            if (!(key = strndup (env[i], cp - env[i]))
                || !(val = json_string (cp + 1)) // can be empty
                || json_object_set_new (obj, key, val)) {
                json_decref (val);
                free (key);
                goto nomem;
            }
            free (key);
        }
    }
    return obj;
nomem:
    errno = ENOMEM;
error:
    ERRNO_SAFE_WRAP (json_decref, obj);
    return NULL;
}

flux_cmd_t *flux_cmd_create (int argc, char *argv[], char **envrn)
{
    flux_cmd_t *cmd;
    json_t *cmdline;
    json_t *env = NULL;

    if (!(cmd = calloc (1, sizeof (*cmd))))
        return NULL;
    if (!(cmdline = cmdline_encode (argc, argv)))
        goto error;
    if (!(env = env_encode (envrn)))
        goto error;
    if (!(cmd->obj = json_pack ("{s:O s:O s:{} s:[]}",
                                "cmdline", cmdline,
                                "env", env,
                                "opts",
                                "channels"))) {
        errno = ENOMEM;
        goto error;
    }
    json_decref (env);
    json_decref (cmdline);
    return (cmd);
error:
    ERRNO_SAFE_WRAP (json_decref, env);
    ERRNO_SAFE_WRAP (json_decref, cmdline);
    flux_cmd_destroy (cmd);
    return NULL;
}

int flux_cmd_argc (const flux_cmd_t *cmd)
{
    json_t *cmdline;

    if (!(cmdline = cmd_get_required_array (cmd, "cmdline")))
        return -1;
    return json_array_size (cmdline);
}

char *flux_cmd_stringify (const flux_cmd_t *cmd)
{
    json_t *cmdline;
    size_t argz_len = 0;
    char *argz = NULL;

    if (!(cmdline = cmd_get_required_array (cmd, "cmdline")))
        return NULL;
    if (cmdline_to_argz  (cmdline, &argz, &argz_len) < 0)
        return NULL;
    if (argz)
        argz_stringify (argz, argz_len, ' ');
    else
        argz = strdup ("");
    return argz;
}

const char *flux_cmd_arg (const flux_cmd_t *cmd, int n)
{
    json_t *cmdline;
    json_t *entry;
    const char *arg;

    if (!(cmdline = cmd_get_required_array (cmd, "cmdline")))
        return NULL;
    if (!(entry = json_array_get (cmdline, n))
        || !(arg = json_string_value (entry))) {
        errno = EINVAL;
        return NULL;
    }
    return arg;
}

int flux_cmd_argv_insert (flux_cmd_t *cmd, int n, const char *entry)
{
    json_t *cmdline;
    json_t *val;

    if (!(cmdline = cmd_get_required_array (cmd, "cmdline"))
        || !argcheck_int_is_bounded (n, 0, json_array_size (cmdline))
        || !argcheck_ptr_is_set (entry))
        return -1;
    if (!(val = json_string (entry))
        ||json_array_insert_new (cmdline, n, val) < 0) {
        json_decref (val);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int flux_cmd_argv_delete (flux_cmd_t *cmd, int n)
{
    json_t *cmdline;

    if (!(cmdline = cmd_get_required_array (cmd, "cmdline"))
        || !argcheck_int_is_bounded (n, 0, json_array_size (cmdline) - 1))
        return -1;
    (void)json_array_remove (cmdline, n); // preconditions already checked
    return 0;
}

int flux_cmd_argv_appendf (flux_cmd_t *cmd, const char *fmt, ...)
{
    va_list ap;
    int rc;
    char *arg;

    if (!argcheck_ptr_is_set (fmt))
        return -1;
    va_start (ap, fmt);
    rc = vasprintf (&arg, fmt, ap);
    va_end (ap);
    if (rc < 0)
        return -1;
    if (flux_cmd_argv_append (cmd, arg) < 0) {
        ERRNO_SAFE_WRAP (free, arg);
        return -1;
    }
    free (arg);
    return 0;
}

int flux_cmd_argv_append (flux_cmd_t *cmd, const char *arg)
{
    json_t *cmdline;
    json_t *o;

    if (!(cmdline = cmd_get_required_array (cmd, "cmdline"))
        || !argcheck_ptr_is_set (arg))
        return -1;
    if (!(o = json_string (arg))
        || json_array_append_new (cmdline, o) < 0) {
        json_decref (o);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int flux_cmd_setenv (flux_cmd_t *cmd,
                            const char *name,
                            const char *val,
                            int overwrite)
{
    json_t *env;
    json_t *o;

    if (!(env = cmd_get_required_object (cmd, "env"))
        || !argcheck_ptr_is_set (name))
        return -1;
    if (!overwrite && json_object_get (env, name) != NULL)
        return 0;
    if (!(o = json_string (val ? val : ""))
        || json_object_set_new (env, name, o) < 0) {
        json_decref (o);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int flux_cmd_setenvf (flux_cmd_t *cmd,
                      int overwrite,
                      const char *name,
                      const char *fmt, ...)
{
    va_list ap;
    char *val;
    int rc;

    if (!argcheck_ptr_is_set (fmt))
        return -1;
    va_start (ap, fmt);
    rc = vasprintf (&val, fmt, ap);
    va_end (ap);
    if (rc < 0)
        return -1;
    if (flux_cmd_setenv (cmd, name, val, overwrite) < 0) {
        ERRNO_SAFE_WRAP (free, val);
        return -1;
    }
    free (val);
    return 0;
}

static bool isa_glob (const char *s)
{
    if (strchr (s, '*') || strchr (s, '?') || strchr (s, '['))
        return true;
    return false;
}

static void delete_glob (json_t *dict, const char *pattern)
{
    json_t *tmp;
    const char *key;
    json_t *value;

    json_object_foreach_safe (dict, tmp, key, value) {
        if (fnmatch (pattern, key, 0) == 0)
            json_object_del (dict, key);
    }
}

void flux_cmd_unsetenv (flux_cmd_t *cmd, const char *name)
{
    json_t *env;

    if (!(env = cmd_get_required_object (cmd, "env"))
        || !argcheck_ptr_is_set (name))
        return;
    if (isa_glob (name))
        delete_glob (env, name);
    else
        json_object_del (env, name);
}

const char *flux_cmd_getenv (const flux_cmd_t *cmd, const char *name)
{
    json_t *env;
    const char *val;

    if (!(env = cmd_get_required_object (cmd, "env"))
        || !argcheck_ptr_is_set (name))
        return NULL;
    if (json_unpack (env, "{s:s}", name, &val) < 0) {
        errno = ENOENT;
        return NULL;
    }
    return val;
}

int flux_cmd_setcwd (flux_cmd_t *cmd, const char *path)
{
    json_t *o;

    if (!argcheck_cmd (cmd) || !argcheck_ptr_is_set (path))
        return -1;
    if (!(o = json_string (path))
        || json_object_set_new (cmd->obj, "cwd", o) < 0) {
        json_decref (o);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

const char *flux_cmd_getcwd (const flux_cmd_t *cmd)
{
    const char *val;

    if (!argcheck_cmd (cmd))
        return NULL;
    if (json_unpack (cmd->obj, "{s:s}", "cwd", &val) < 0) {
        errno = ENOENT;
        return NULL;
    }
    return val;
}

static bool array_value_isnt (json_t *a, const char *name)
{
    size_t index;
    json_t *entry;
    json_array_foreach (a, index, entry) {
        const char *val;
        if ((val = json_string_value (entry)) && streq (val, name)) {
            errno = EEXIST;
            return false;
        }
    }
    return true;
}

int flux_cmd_add_channel (flux_cmd_t *cmd, const char *name)
{
    json_t *channels;
    json_t *o;

    if (!(channels = cmd_get_required_array (cmd, "channels"))
        || !argcheck_ptr_is_set (name)
        || !array_value_isnt (channels, name))
        return -1;
    if (!(o = json_string (name))
        || json_array_append_new (channels, o) < 0) {
        json_decref (o);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static bool array_object_key_isnt (json_t *a, const char *key, const char *name)
{
    size_t index;
    json_t *entry;
    json_array_foreach (a, index, entry) {
        const char *val;
        if (json_unpack (entry, "{s:s}", key, &val) == 0 && streq (val, name)) {
            errno = EEXIST;
            return false;
        }
    }
    return true;
}

int flux_cmd_add_message_channel (flux_cmd_t *cmd,
                                  const char *name,
                                  const char *uri)
{
    json_t *msgchans;
    json_t *o;

    if (!argcheck_cmd (cmd)
        || !argcheck_ptr_is_set (name)
        || !argcheck_ptr_is_set (uri))
        return -1;
    // msgchans is optional
    if (!(msgchans = json_object_get (cmd->obj, "msgchans"))) {
        if (!(msgchans = json_array ())
            || json_object_set_new (cmd->obj, "msgchans", msgchans) < 0) {
            json_decref (msgchans);
            errno = ENOMEM;
            return -1;
        }
    }
    if (!array_object_key_isnt (msgchans, "name", name))
        return -1;
    if (!(o = json_pack ("{s:s s:s}",
                         "name", name,
                         "uri", uri))
        || json_array_append_new (msgchans, o) < 0) {
        json_decref (o);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int flux_cmd_setopt (flux_cmd_t *cmd, const char *name, const char *val)
{
    json_t *opts;
    json_t *o;

    if (!(opts = cmd_get_required_object (cmd, "opts"))
        || !argcheck_ptr_is_set (name)
        || !argcheck_ptr_is_set (val))
        return -1;
    if (!(o = json_string (val))
        || json_object_set_new (opts, name, o) < 0) {
        json_decref (o);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

const char *flux_cmd_getopt (flux_cmd_t *cmd, const char *name)
{
    json_t *opts;
    json_t *o;
    const char *val;

    if (!(opts = cmd_get_required_object (cmd, "opts"))
        || !argcheck_ptr_is_set (name))
        return NULL;
    if (!(o = json_object_get (opts, name))
        || !(val = json_string_value (o))) {
        errno = ENOENT;
        return NULL;
    }
    return val;
}

flux_cmd_t *flux_cmd_copy (const flux_cmd_t *cmd)
{
    flux_cmd_t *cpy;

    if (!argcheck_cmd (cmd))
        return NULL;
    if (!(cpy = calloc (1, sizeof (*cpy))))
        return NULL;
    if (!(cpy->obj = json_deep_copy (cmd->obj))) {
        free (cpy);
        errno = ENOMEM;
        return NULL;
    }
    return cpy;
}

flux_cmd_t *cmd_fromjson (json_t *obj, json_error_t *errp)
{
    flux_cmd_t *cmd;

    if (!argcheck_ptr_is_set (obj))
        return NULL;
    if (!(cmd = calloc (1, sizeof (*cmd))))
        return NULL;
    if (!(cmd->obj = json_deep_copy (obj))) {
        errno = ENOMEM;
        goto error;
    }
    if (!argcheck_cmd_json (cmd, errp))
        goto error;
    return cmd;
error:
    flux_cmd_destroy (cmd);
    return NULL;
}

json_t *cmd_tojson (const flux_cmd_t *cmd)
{
    json_t *cpy;

    if (!argcheck_cmd (cmd))
        return NULL;
    if (!(cpy = json_deep_copy (cmd->obj))) {
        errno = ENOMEM;
        return NULL;
    }
    return cpy;
}

char **cmd_env_expand (flux_cmd_t *cmd)
{
    json_t *env;

    if (!(env = cmd_get_required_object (cmd, "env")))
        return NULL;
    return env_decode (env);
}

char **cmd_argv_expand (flux_cmd_t *cmd)
{
    json_t *cmdline;

    if (!(cmdline = cmd_get_required_array (cmd, "cmdline")))
        return NULL;
    return cmdline_decode (cmdline);
}

int cmd_set_env (flux_cmd_t *cmd, char **envrn)
{
    json_t *env;

    if (!argcheck_cmd (cmd) || !argcheck_ptr_is_set (envrn))
        return -1;
    if (!(env = env_encode (envrn)))
        return -1;
    if (json_object_set_new (cmd->obj, "env", env) < 0) {
        json_decref (env);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

json_t *cmd_get_channels (flux_cmd_t *cmd)
{
    return cmd_get_required_array (cmd, "channels");
}

json_t *cmd_get_msgchans (flux_cmd_t *cmd)
{
    return cmd_get_required_array (cmd, "msgchans");
}

static bool match_substring (const char *key, const char **substrings)
{
    while ((*substrings)) {
        if (strstr (key, (*substrings)))
            return true;
        substrings++;
    }
    return false;
}

// used in unit tests
int cmd_find_opts (const flux_cmd_t *cmd, const char **substrings)
{
    json_t *opts;
    const char *key;
    json_t *value;

    if (!(opts = cmd_get_required_object (cmd, "opts"))
        || !argcheck_ptr_is_set (substrings))
        return 0;
    json_object_foreach (opts, key, value) {
        if (match_substring (key, substrings))
            return 1;
    }
    return 0;
}

/*
 * vi: ts=4 sw=4 expandtab
 */
