/************************************************************\
 * Copyright 2025 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* scratchpad.c - broker-scope store with atomic read-modify-write
 *
 * This uses load-link/store-conditional style atomicity:
 *   https://en.wikipedia.org/wiki/Load-link/store-conditional
 *
 * Implemented using messages, this technique is not scalable to
 * large data structures or many readers/writers.  However, it allows
 * any data structure to be atomically updated while remaining opaque
 * to the scratchpad service.
 *
 * +--------+
 * | Design |
 * +--------+
 *
 * The store is represented internally as a JSON dictionary.
 * Each entry has the following structure:
 *   {"version":i "data":o}
 * Special case: a non-existent key is implicitly looked up as
 *   {"version":0 "data":null}.
 *
 * There are two main operators: load-link (LL) and store-conditional (SC):
 *
 * LL fetches an entry by key.
 *   > {"key":s}
 *   < {"version":i "data":o}
 *
 * SC updates an entry by key, incrementing the stored version.
 *   > {"key":s "version":i "data" o}
 *
 * The SC request includes the key version upon which the update was based.
 * If the stored key version == the SC request version, the update succeeds:
 * the stored data is replaced with the SC request data, and the stored
 * version is incremented.  If the stored key version != the SC request
 * version, a race has occurred and the update fails.
 *
 * Consider a json array named 'foo' with multiple appenders.  Each might
 * implement the following:
 *
 *   json_t *update_array (json_t *data, json_t *element)
 *   {
 *     json_t *o = json_is_null (data) ? json_array () : json_copy (data);
 *     json_array_append (o, element);
 *     return o;
 *   }
 *
 *   do {
 *     int version;
 *     json_t *data;
 *
 *     LL (h, "foo", &version, &data)
 *
 *     json_t *new_data = update_array (data, element);
 *   } while (SC (h, "foo", version, new_data) < 0)
 *
 * The LL+SC are simply retried until the SC completes successfully.
 *
 * +-----------+
 * | Design II |
 * +-----------+
 *
 * Refining the implementation to fit more comfortably to the Flux reactive
 * messaging architecture, a streaming RPC version of SC is implemented:
 *
 * SC-stream updates an entry by key:
 *   > {"key":s "version":i "data":o}
 *   < {"version":i "data":o} (on failure, an LL response)
 *   < ENODATA (on success)
 *
 * SC-retry retries an active SC-stream (no response)
 *   > {"matchtag":i "version":i "data":o}
 *
 * Each SC-retry triggers a new response to the SC-stream it references.
 *
 * The simplified atomic array append above becomes:
 *
 *   int version = 0;
 *   json_t *new_data = update_array (json_null (), element):
 *   flux_future_t *f = SC_stream (h, "foo", version, new_data);
 *
 *   while (SC_stream_get (f, &version, &data) == 0) {
 *     new_data = update_array (data, element):
 *     SC_retry (f, version, new_data);
 *     flux_future_reset (f);
 *   }
 *
 * This approach saves
 * - sending an LL request in lock-step each time the update fails
 * - the initial LL request if the key happens not to exist
 *
 * Since the streaming RPC uses a Flux future, the loop can be converted to
 * a continuation function for asynchronous execution.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>

#include "ccan/str/str.h"

struct scratchpad {
    flux_t *h;
    flux_msg_handler_t **handlers;
    json_t *data;
    int version;
    struct flux_msglist *requests;
};

/* Special case: key "." is read-only and fetches the entire scratchpad.
 * If the scratchpad is empty, {"version":0 "data":null} is returned.
 */
static void lookup (struct scratchpad *ctx,
                    const char *key,
                    int *version,
                    json_t **data)
{
    if (streq (key, ".")) {
        *version = ctx->version;
        *data = ctx->version == 0 ? json_null () : ctx->data;
    }
    else if (json_unpack (ctx->data,
                          "{s:{s:i s:o}}",
                          key,
                          "version", version,
                          "data", data) < 0) {
        *version = 0;
        *data = json_null ();
    }
}

static int update (struct scratchpad *ctx,
                   const char *key,
                   int version,
                   json_t *data)
{
    if (streq (key, ".")) {
        errno = EROFS;
        return -1;
    }
    json_t *entry;
    if (!(entry = json_pack ("{s:i s:O}", "version", version, "data", data))
        || json_object_set_new (ctx->data, key, entry) < 0) {
        json_decref (entry);
        errno = ENOMEM;
        return -1;
    }
    ctx->version++;
    return 0;
}

static void ll_cb (flux_t *h,
                   flux_msg_handler_t *mh,
                   const flux_msg_t *msg,
                   void *arg)
{
    struct scratchpad *ctx = arg;
    const char *key;
    int version;
    json_t *data;

    if (flux_request_unpack (msg, NULL, "{s:s}", "key", &key) < 0)
        goto error;
    lookup (ctx, key, &version, &data);
    if (flux_respond_pack (h,
                           msg,
                           "{s:i s:O}",
                           "version", version,
                           "data", data) < 0)
        flux_log_error (h, "error responding to ll request");
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "error responding to ll request");
}

static void sc_cb (flux_t *h,
                   flux_msg_handler_t *mh,
                   const flux_msg_t *msg,
                   void *arg)
{
    struct scratchpad *ctx = arg;
    const char *key;
    int version;
    json_t *data;
    int curversion;
    json_t *curdata;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:s s:i s:o}",
                             "key", &key,
                             "version", &version,
                             "data", &data) < 0)
        goto error;
    lookup (ctx, key, &curversion, &curdata);
    if (curversion != version) {
        errno = EDEADLK;
        goto error;
    }
    if (update (ctx, key, version + 1, data) < 0)
        goto error;
    if (flux_respond (h, msg, NULL) < 0)
        flux_log_error (h, "error responding to sc request");
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "error responding to sc request");
}

static void sc_stream_cb (flux_t *h,
                          flux_msg_handler_t *mh,
                          const flux_msg_t *msg,
                          void *arg)
{
    struct scratchpad *ctx = arg;
    const char *key;
    int version;
    json_t *data;
    int curversion;
    json_t *curdata;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:s s:i s:o}",
                             "key", &key,
                             "version", &version,
                             "data", &data) < 0)
        goto error;
    if (!flux_msg_is_streaming (msg)) {
        errno = EINVAL;
        goto error;
    }
    lookup (ctx, key, &curversion, &curdata);
    if (curversion == version) {
        if (update (ctx, key, version + 1, data) < 0)
            goto error;
        errno = ENODATA;
        goto error;
    }
    if (flux_msglist_append (ctx->requests, msg) < 0)
        goto error;
    if (flux_respond_pack (h,
                           msg,
                           "{s:i s:O}",
                           "version", curversion,
                           "data", curdata) < 0)
        flux_log_error (h, "error responding to sc-stream request");
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "error responding to sc-stream request");
}

static const flux_msg_t *find_request (struct scratchpad *ctx,
                                       const flux_msg_t *msg)
{
    const flux_msg_t *request;
    request = flux_msglist_first (ctx->requests);
    while (request) {
        if (flux_cancel_match (msg, request))
            return request;
        request = flux_msglist_next (ctx->requests);
    }
    return NULL;
}

static void sc_retry_cb (flux_t *h,
                         flux_msg_handler_t *mh,
                         const flux_msg_t *msg,
                         void *arg)
{
    struct scratchpad *ctx = arg;
    int matchtag;
    int version;
    json_t *data;
    const flux_msg_t *request;
    const char *key;
    int curversion;
    json_t *curdata;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:i s:i s:o}",
                             "matchtag", &matchtag,
                             "version", &version,
                             "data", &data) < 0)
        return;
    if (!flux_msg_is_noresponse (msg)) {
        if (flux_respond_error (h,
                                msg,
                                EPROTO,
                                "NORESPONSE flag is missing from request"))
            flux_log_error (h, "error responding to delete request");
        return;
    }
    if (!(request = find_request (ctx, msg))
        || flux_request_unpack (request, NULL, "{s:s}", "key", &key) < 0)
        return;
    lookup (ctx, key, &curversion, &curdata);
    if (curversion == version) {
        if (update (ctx, key, version + 1, data) < 0)
            goto error;
        errno = ENODATA;
        goto error;
    }
    if (flux_respond_pack (h,
                           request,
                           "{s:i s:O}",
                           "version", curversion,
                           "data", curdata) < 0)
        flux_log_error (h, "error responding to sc-stream request");
    return;
error:
    if (flux_respond_error (h, request, errno, NULL) < 0)
        flux_log_error (h, "error responding to sc-stream request");
}

static void delete_cb (flux_t *h,
                       flux_msg_handler_t *mh,
                       const flux_msg_t *msg,
                       void *arg)
{
    struct scratchpad *ctx = arg;
    const char *key;

    if (!flux_msg_is_noresponse (msg)) {
        if (flux_respond_error (h,
                                msg,
                                EPROTO,
                                "NORESPONSE flag is missing from request"))
            flux_log_error (h, "error responding to delete request");
        return;
    }
    if (flux_request_unpack (msg, NULL, "{s:s}", "key", &key) < 0)
        return;
    if (json_object_get (ctx->data, key)) {
        (void)json_object_del (ctx->data, key);
        ctx->version++;
    }
}

static struct flux_msg_handler_spec htab[] = {
    {
        FLUX_MSGTYPE_REQUEST,
        "ll",
        ll_cb,
        FLUX_ROLE_USER,
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "sc",
        sc_cb,
        0,
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "sc-stream",
        sc_stream_cb,
        0,
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "sc-retry",
        sc_retry_cb,
        0,
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "delete",
        delete_cb,
        0,
    },
    FLUX_MSGHANDLER_TABLE_END,
};

static void scratchpad_fini (struct scratchpad *ctx)
{
    if (ctx) {
        int saved_errno = errno;
        json_decref (ctx->data);
        if (ctx->requests) {
            const flux_msg_t *msg;
            while ((msg = flux_msglist_pop (ctx->requests))) {
                if (flux_respond_error (ctx->h, msg, ENOSYS, NULL) < 0)
                    flux_log_error (ctx->h,
                                    "error responding to sc-stream request");
                flux_msg_decref (msg);
            }
            flux_msglist_destroy (ctx->requests);
        }
        flux_msg_handler_delvec (ctx->handlers);
        errno = saved_errno;
    }
}

static int scratchpad_init (struct scratchpad *ctx, flux_t *h)
{
    ctx->h = h;
    if (!(ctx->data = json_object ())) {
        errno = ENOMEM;
        return -1;
    }
    if (!(ctx->requests = flux_msglist_create ()))
        return -1;
    const char *name = flux_aux_get (h, "flux::name");
    return flux_msg_handler_addvec_ex (h, name, htab, ctx, &ctx->handlers);
}

int mod_main (flux_t *h, int argc, char **argv)
{
    struct scratchpad ctx = {0};
    int rc = -1;

    if (scratchpad_init (&ctx, h) < 0)
        goto done;
    if (flux_reactor_run (flux_get_reactor (h), 0) < 0) {
        flux_log_error (h, "reactor aborted");
        goto done;
    }
    rc = 0;
done:
    scratchpad_fini (&ctx);
    return rc;
}

// vi:ts=4 sw=4 expandtab
