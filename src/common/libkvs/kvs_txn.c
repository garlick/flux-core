/*****************************************************************************\
 *  Copyright (c) 2017 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>
#include <string.h>

#include "kvs_txn_private.h"
#include "jansson_dirent.h"

struct flux_kvs_txn {
    json_t *ops;
    int cursor;
};

void flux_kvs_txn_destroy (flux_kvs_txn_t *txn)
{
    if (txn) {
        int saved_errno = errno;
        json_decref (txn->ops);
        free (txn);
        errno = saved_errno;
    }
}

flux_kvs_txn_t *flux_kvs_txn_create (void)
{
    flux_kvs_txn_t *txn = calloc (1, sizeof (*txn));
    if (!txn) {
        errno = ENOMEM;
        goto error;
    }
    if (!(txn->ops = json_array ())) {
        errno = ENOMEM;
        goto error;
    }
    return txn;
error:
    flux_kvs_txn_destroy (txn);
    return NULL;
}

static int validate_flags (int flags, int allowed)
{
    if ((flags & allowed) != flags) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int validate_op (json_t *op)
{
    const char *key;
    json_t *dirent = NULL;

    if (json_unpack (op, "{s:s}", "key", &key) < 0)
        goto error;
    if (strlen (key) == 0)
        goto error;
    if (json_unpack (op, "{s:n}", "dirent") == 0)
        ; // unlink sets dirent NULL
    else if (json_unpack (op, "{s:o}", "dirent", &dirent) == 0) {
        if (j_dirent_validate (dirent) < 0)
            goto error;
    } else
        goto error;
    return 0;
error:
    errno = EINVAL;
    return -1;
}

int flux_kvs_txn_put (flux_kvs_txn_t *txn, int flags,
                      const char *key, const char *json_str)
{
    json_t *val;
    json_t *op = NULL;

    if (!txn || !key) {
        errno = EINVAL;
        goto error;
    }
    if (validate_flags (flags, FLUX_KVS_TREEOBJ) < 0)
        goto error;
    if (!json_str)
        return flux_kvs_txn_unlink (txn, flags, key);
    if (!(val = json_loads (json_str, JSON_DECODE_ANY, NULL))) {
        errno = EINVAL;
        goto error;
    }
    if ((flags & FLUX_KVS_TREEOBJ))
        op = json_pack ("{s:s s:o}", "key", key,
                                     "dirent", val);
    else
        op = json_pack ("{s:s s:{s:o}}", "key", key,
                                         "dirent", "FILEVAL", val);
    if (!op) {
        json_decref (val);
        errno = ENOMEM;
        goto error;
    }
    if (validate_op (op) < 0) {
        json_decref (op);
        goto error;
    }
    if (json_array_append_new (txn->ops, op) < 0) {
        json_decref (op);
        errno = ENOMEM;
        goto error;
    }
    return 0;
error:
    return -1;
}

int flux_kvs_txn_pack (flux_kvs_txn_t *txn, int flags,
                       const char *key, const char *fmt, ...)
{
    va_list ap;
    json_t *val;
    json_t *op = NULL;

    if (!txn || !key | !fmt) {
        errno = EINVAL;
        goto error;
    }
    if (validate_flags (flags, FLUX_KVS_TREEOBJ) < 0)
        goto error;
    va_start (ap, fmt);
    val = json_vpack_ex (NULL, 0, fmt, ap);
    va_end (ap);
    if (!val) {
        errno = EINVAL;
        goto error;
    }
    if ((flags & FLUX_KVS_TREEOBJ))
        op = json_pack ("{s:s s:o}", "key", key,
                                     "dirent", val);
    else
        op = json_pack ("{s:s s:{s:o}}", "key", key,
                                         "dirent", "FILEVAL", val);
    if (!op) {
        json_decref (val);
        errno = ENOMEM;
        goto error;
    }
    if (validate_op (op) < 0) {
        json_decref (op);
        goto error;
    }
    if (json_array_append_new (txn->ops, op) < 0) {
        json_decref (op);
        errno = ENOMEM;
        goto error;
    }
    return 0;
error:
    return -1;
}

int flux_kvs_txn_mkdir (flux_kvs_txn_t *txn, int flags,
                        const char *key)
{
    json_t *op;

    if (!txn || !key) {
        errno = EINVAL;
        goto error;
    }
    if (validate_flags (flags, 0) < 0)
        goto error;
    if (!(op = json_pack ("{s:s s:{s:{}}}", "key", key,
                                            "dirent", "DIRVAL"))) {
        errno = ENOMEM;
        goto error;
    }
    if (validate_op (op) < 0) {
        json_decref (op);
        goto error;
    }
    if (json_array_append_new (txn->ops, op) < 0) {
        json_decref (op);
        errno = ENOMEM;
        goto error;
    }
    return 0;
error:
    return -1;
}

int flux_kvs_txn_unlink (flux_kvs_txn_t *txn, int flags,
                         const char *key)
{
    json_t *op;

    if (!txn || !key) {
        errno = EINVAL;
        goto error;
    }
    if (validate_flags (flags, 0) < 0)
        goto error;
    if (!(op = json_pack ("{s:s s:n}", "key", key,
                                       "dirent"))) {
        errno = ENOMEM;
        goto error;
    }
    if (validate_op (op) < 0) {
        json_decref (op);
        goto error;
    }
    if (json_array_append_new (txn->ops, op) < 0) {
        json_decref (op);
        errno = ENOMEM;
        goto error;
    }
    return 0;
error:
    return -1;
}

int flux_kvs_txn_symlink (flux_kvs_txn_t *txn, int flags,
                          const char *key, const char *target)
{
    json_t *op;

    if (!txn || !key | !target) {
        errno = EINVAL;
        goto error;
    }
    if (validate_flags (flags, 0) < 0)
        goto error;
    if (!(op = json_pack ("{s:s s:{s:s}}", "key", key,
                                           "dirent", "LINKVAL", target))) {
        errno = EINVAL;
        goto error;
    }
    if (validate_op (op) < 0) {
        json_decref (op);
        goto error;
    }
    if (json_array_append_new (txn->ops, op) < 0) {
        json_decref (op);
        errno = ENOMEM;
        goto error;
    }
    return 0;
error:
    return -1;
}

/* accessors for KVS internals and unit tests
 */
int txn_get (flux_kvs_txn_t *txn, int request, void *arg)
{
    switch (request) {
        case TXN_GET_FIRST:
            txn->cursor = 0;
            if (arg)
                *(json_t **)arg = json_array_get (txn->ops, txn->cursor);
            txn->cursor++;
            break;
        case TXN_GET_NEXT:
            if (arg)
                *(json_t **)arg = json_array_get (txn->ops, txn->cursor);
            txn->cursor++;
            break;
        case TXN_GET_ALL:
            if (arg)
                *(json_t **)arg = txn->ops;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */