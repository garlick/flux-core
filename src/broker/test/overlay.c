/************************************************************\
 * Copyright 2020 Lawrence Livermore National Security, LLC
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
#include <string.h>
#include <flux/core.h>
#include <czmq.h>

#include "src/common/libtap/tap.h"
#include "src/common/libtestutil/util.h"
#include "src/common/libutil/stdlog.h"
#include "src/common/libutil/unlink_recursive.h"

#include "src/broker/overlay.h"
#include "src/broker/attr.h"

static zlist_t *logs;

struct context {
    struct overlay *ov;
    flux_t *h;
    attr_t *attrs;
    char name[32];
    int rank;
    int size;
};

void clear_list (zlist_t *list)
{
    char *s;
    while ((s = zlist_pop (list)))
        free (s);
}

int match_list (zlist_t *list, const char *key)
{
    char *s;
    int count = 0;

    s = zlist_first (list);
    while (s) {
        if (strstr (s, key) != NULL)
            count++;
        s = zlist_next (list);
    }
    return count;
}

void check_attr (struct context *ctx, const char *k, const char *v)
{
    const char *val;

    ok (attr_get (ctx->attrs, k, &val, NULL)  == 0
        && ((v == NULL && val == NULL)
            || (v != NULL && val != NULL && !strcmp (v, val))),
        "%s: %s=%s", ctx->name, k, v ? v : "NULL");
}

void ctx_destroy (struct context *ctx)
{
    attr_destroy (ctx->attrs);
    overlay_destroy (ctx->ov);
    free (ctx);
}

struct context *ctx_create (flux_t *h, const char *name, int size,  int rank)
{
    struct context *ctx;
    const char *temp = getenv ("TMPDIR");
    if (!temp)
        temp = "/tmp";

    if (!(ctx = calloc (1, sizeof (*ctx))))
        BAIL_OUT ("calloc failed");
    ctx->h = h;
    ctx->size = size;
    ctx->rank = rank;
    snprintf (ctx->name, sizeof (ctx->name), "%s-%d", name, rank);
    if (!(ctx->ov = overlay_create (h)))
        BAIL_OUT ("overlay_create");
    if (!(ctx->attrs = attr_create ()))
        BAIL_OUT ("attr_create failed");

    return ctx;
}

int single_init_cb (struct overlay *ov, void *arg)
{
    struct context *ctx = arg;
    // get rundir attr
    // create rank subidr
    // set broker.rundir attr
    // set local-uri attr to local://${broker.rundir}/local
    flux_log (ctx->h, LOG_INFO, "single_init_cb called");
    return 0;
}


void single (flux_t *h)
{
    struct context *ctx = ctx_create (h, "single", 1, 0);;
    flux_msg_t *msg;

    overlay_set_init_callback (ctx->ov, single_init_cb, ctx);
    ok (overlay_init (ctx->ov, 1, 0, 2) == 0,
        "%s: overlay_init size=1 rank=0 tbon_k=2 works", ctx->name);
    ok (match_list (logs, "single_init_cb called") == 1,
        "%s: overlay_init triggered init callback", ctx->name);

    ok (overlay_get_size (ctx->ov) == 1,
        "%s: overlay_get_size returns 1", ctx->name);
    ok (overlay_get_rank (ctx->ov) == 0,
        "%s: overlay_get_rank returns 0", ctx->name);

    ok (overlay_register_attrs (ctx->ov, ctx->attrs) == 0,
        "%s: overlay_register_attrs works", ctx->name);
    check_attr (ctx, "tbon.parent-endpoint", NULL);
    check_attr (ctx, "rank", "0");
    check_attr (ctx, "size", "1");
    check_attr (ctx, "tbon.arity", "2");
    check_attr (ctx, "tbon.level", "0");
    check_attr (ctx, "tbon.maxlevel", "0");
    check_attr (ctx, "tbon.descendants", "0");

    /* No parent uri.
     * No bind uri because no children
     */
    ok (overlay_get_parent_uri (ctx->ov) == NULL,
        "%s: overlay_get_parent_uri returned NULL", ctx->name);
    ok (overlay_get_bind_uri (ctx->ov) == NULL,
        "%s: overlay_get_bind_uri returned NULL", ctx->name);

    /* Try to mcast an event downstream */
    if (!(msg = flux_event_encode ("foo", NULL)))
        BAIL_OUT ("flux_event_encode failed");
    overlay_mcast_child (ctx->ov, msg);
    flux_msg_decref (msg);

    /* Try to send a response to non-existent downstream peer */
    if (!(msg = flux_response_encode ("foo", NULL)))
        BAIL_OUT ("flux_request_encode failed");
    ok (overlay_sendmsg_child (ctx->ov, msg) < 0 && errno == EINVAL,
        "%s: overlay_sendmsg_child fails with EINVAL", ctx->name);
    flux_msg_decref (msg);

    ok (overlay_get_child_peer_count (ctx->ov) == 0,
        "%s: overlay_get_child_peer_count returns 0", ctx->name);

    ctx_destroy (ctx);
}

int duo_init_cb (struct overlay *ov, void *arg)
{
    struct context *ctx = arg;

    flux_log (ctx->h, LOG_INFO, "duo_init_cb called for %s", ctx->name);
    return 0;
}

void socket_cb (struct overlay *ov, void *arg)
{
    struct context *ctx = arg;
    flux_reactor_stop (flux_get_reactor (ctx->h));
}

void timeout_cb (flux_reactor_t *r, flux_watcher_t *w, int revents, void *arg)
{
    errno = ETIMEDOUT;
    flux_reactor_stop_error (r);
}

flux_msg_t *recvmsg_child_timeout (struct context *ctx, double timeout)
{
    flux_reactor_t *r = flux_get_reactor (ctx->h);
    flux_watcher_t *w;

    overlay_set_child_cb (ctx->ov, socket_cb, ctx);
    if (!(w = flux_timer_watcher_create (r, timeout, 0., timeout_cb, ctx)))
        BAIL_OUT ("flux_timer_watcher_create failed");
    flux_watcher_start (w);

    if (flux_reactor_run (r, 0) < 0)
        return NULL;
    return overlay_recvmsg_child (ctx->ov);
}

flux_msg_t *recvmsg_parent_timeout (struct context *ctx, double timeout)
{
    flux_reactor_t *r = flux_get_reactor (ctx->h);
    flux_watcher_t *w;

    overlay_set_parent_cb (ctx->ov, socket_cb, ctx);
    if (!(w = flux_timer_watcher_create (r, timeout, 0., timeout_cb, ctx)))
        BAIL_OUT ("flux_timer_watcher_create failed");
    flux_watcher_start (w);

    if (flux_reactor_run (r, 0) < 0)
        return NULL;
    return overlay_recvmsg_parent (ctx->ov);
}

void trio (flux_t *h)
{
    struct context *ctx[2];
    int size = 3;
    int k_ary = 2;
    char parent_uri[64];
    const char *server_pubkey;
    const char *client_pubkey;
    const char *tmp;
    flux_msg_t *msg;
    flux_msg_t *msg2;
    const char *topic;
    char uuid[16];
    zsock_t *zsock_none;
    zsock_t *zsock_curve;
    zcert_t *cert;

    ctx[0] = ctx_create (h, "trio", size, 0);

    ok (overlay_init (ctx[0]->ov, size, 0, k_ary) == 0,
        "%s: overlay_init works", ctx[0]->name);

    ok ((server_pubkey = overlay_cert_pubkey (ctx[0]->ov)) != NULL,
        "%s: overlay_cert_pubkey works", ctx[0]->name);

    snprintf (parent_uri, sizeof (parent_uri), "ipc://@%s", ctx[0]->name);
    ok (overlay_bind (ctx[0]->ov, parent_uri) == 0,
        "%s: overlay_bind %s works", ctx[0]->name, parent_uri);

    ctx[1] = ctx_create (h, "trio", size, 1);

    ok (overlay_init (ctx[1]->ov, size, 1, k_ary) == 0,
        "%s: overlay_init works", ctx[1]->name);

    ok ((client_pubkey = overlay_cert_pubkey (ctx[1]->ov)) != NULL,
        "%s: overlay_cert_pubkey works", ctx[1]->name);
    ok (overlay_set_parent_uri (ctx[1]->ov, parent_uri) == 0,
        "%s: overlay_set_parent_uri %s works", ctx[1]->name, parent_uri);
    tmp = overlay_get_parent_uri (ctx[1]->ov);
    ok (tmp != NULL && !strcmp (tmp, parent_uri),
        "%s: overlay_get_parent_uri returns same string", ctx[1]->name);
    ok (overlay_set_parent_pubkey (ctx[1]->ov, server_pubkey) == 0,
        "%s: overlay_set_parent_pubkey works", ctx[1]->name);

    ok (overlay_authorize (ctx[0]->ov, ctx[0]->name, client_pubkey) == 0,
        "%s: overlay_authorize %s works", ctx[0]->name, client_pubkey);
    ok (overlay_connect (ctx[1]->ov) == 0,
        "%s: overlay_connect works", ctx[1]->name);

    errno = 0;
    ok (overlay_authorize (ctx[0]->ov, "foo", "1234") < 0 && errno == EINVAL,
        "overlay_authorize with short pubkey fails with EINVAL");

    /* Send 1->0
     */
    if (!(msg = flux_request_encode ("meep", NULL)))
        BAIL_OUT ("flux_request_encode failed");
    ok (overlay_sendmsg_parent (ctx[1]->ov, msg) == 0,
        "%s: overlay_sendmsg_parent works", ctx[1]->name);
    flux_msg_destroy (msg);

    msg = recvmsg_child_timeout (ctx[0], 5);
    ok (msg != NULL,
        "%s: overlay_recvmsg_child works", ctx[0]->name);
    ok (flux_msg_get_topic (msg, &topic) == 0 && !strcmp (topic, "meep"),
        "%s: received message has expected topic", ctx[0]->name);
    flux_msg_destroy (msg);

    /* Send 0->1
     */
    if (!(msg = flux_response_encode ("moop", NULL)))
        BAIL_OUT ("flux_response_encode failed");
    snprintf (uuid, sizeof (uuid), "%d", 1);
    if (flux_msg_push_route (msg, uuid) < 0) // direct router socket to 1
        BAIL_OUT ("flux_msg_push_route failed");
    ok (overlay_sendmsg_child (ctx[0]->ov, msg) == 0,
        "%s: overlay_sendmsg_child works", ctx[0]->name);
    flux_msg_destroy (msg);

    msg = recvmsg_parent_timeout (ctx[1], 5);
    ok (msg != NULL,
        "%s: overlay_recvmsg_parent works", ctx[1]->name);
    ok (flux_msg_get_topic (msg, &topic) == 0 && !strcmp (topic, "moop"),
        "%s: received message has expected topic", ctx[1]->name);
    flux_msg_destroy (msg);

    errno = 0;
    ok (overlay_bind (ctx[1]->ov, "ipc://@foo") < 0 && errno == EINVAL,
        "%s: second overlay_bind in proc fails with EINVAL", ctx[0]->name);

    /* Various tests of rank 2 without proper authorization.
     * First a baseline - resend 1->0 and make sure timed recv works.
     * Test message will be reused below.
     */
    if (!(msg = flux_request_encode ("erp", NULL)))
        BAIL_OUT ("flux_request_encode failed");
    ok (overlay_sendmsg_parent (ctx[1]->ov, msg) == 0,
        "%s: overlay_sendmsg_parent works", ctx[1]->name);
    msg2 = recvmsg_child_timeout (ctx[0], 5);
    ok (msg2 != NULL,
        "%s: recvmsg_child_timeout received test message", ctx[0]->name);
    flux_msg_decref (msg2);
    errno = 0;
    ok (recvmsg_child_timeout (ctx[0], 0.1) == NULL  && errno == ETIMEDOUT,
        "%s: recvmsg_child_timeout timed out as expected", ctx[0]->name);

    /* 1) No security
     */
    if (!(zsock_none = zsock_new_dealer (NULL)))
        BAIL_OUT ("zsock_new_dealer failed");
    zsock_set_identity (zsock_none, "2");
    ok (zsock_connect (zsock_none, "%s", parent_uri) == 0,
        "none-2: zsock_connect %s (no security) works", parent_uri);
    ok (flux_msg_sendzsock (zsock_none, msg) == 0,
        "none-2: zsock_msg_sendzsock works");

    /* 2) Curve, and correct server publc key, but client public key
     * was not authorized
     */
    if (!(zsock_curve = zsock_new_dealer (NULL)))
        BAIL_OUT ("zsock_new_dealer failed");
    if (!(cert = zcert_new ()))
        BAIL_OUT ("zcert_new failed");
    zsock_set_zap_domain (zsock_curve, "flux");
    zcert_apply (cert, zsock_curve);
    zsock_set_curve_serverkey (zsock_curve, server_pubkey);
    zsock_set_identity (zsock_curve, "2");
    zcert_destroy (&cert);
    ok (zsock_connect (zsock_curve, "%s", parent_uri) == 0,
        "curve-2: zsock_connect %s works", parent_uri);
    ok (flux_msg_sendzsock (zsock_curve, msg) == 0,
        "curve-2: flux_msg_sendzsock works");

    /* Neither of the above attempts should have gotten a message through.
     */
    errno = 0;
    ok (recvmsg_child_timeout (ctx[0], 1.0) == NULL  && errno == ETIMEDOUT,
        "%s: no messages received within 1.0s", ctx[0]->name);

    flux_msg_decref (msg);
    zsock_destroy (&zsock_none);
    zsock_destroy (&zsock_curve);

    ctx_destroy (ctx[1]);
    ctx_destroy (ctx[0]);
}

/* Probe some possible failure cases
 */
void wrongness (flux_t *h)
{
    struct overlay *ov;

    errno = 0;
    ok (overlay_create (NULL) == NULL && errno == EINVAL,
        "overlay_create h=NULL fails with EINVAL");

    if (!(ov = overlay_create (h)))
        BAIL_OUT ("overlay_create failed");

    errno = 0;
    ok (overlay_bind (ov, "ipc://@foobar") < 0 && errno == EINVAL,
        "overlay_bind fails if called before rank is known");

    overlay_destroy (ov);
}

void diag_logger (const char *buf, int len, void *arg)
{
    struct stdlog_header hdr;
    const char *msg;
    int msglen, severity;
    char *s;

    if (stdlog_decode (buf, len, &hdr, NULL, NULL, &msg, &msglen) < 0)
        BAIL_OUT ("stdlog_decode failed");
    severity = STDLOG_SEVERITY (hdr.pri);
    if (asprintf (&s,
                  "%s: %.*s\n",
                  stdlog_severity_to_string (severity),
                  msglen, msg) < 0)
        BAIL_OUT ("asprintf failed");
    diag (s);
    if (zlist_append (logs, s) < 0)
        BAIL_OUT ("zlist_append failed");
}

int main (int argc, char *argv[])
{
    flux_t *h;

    plan (NO_PLAN);

    if (!zsys_init ())
        BAIL_OUT ("zsys_init failed");
    zsys_set_linger (5);

    if (!(logs = zlist_new ()))
        BAIL_OUT ("zlist_new failed");
    if (!(h = loopback_create (0)))
        BAIL_OUT ("loopback_create failed");
    if (flux_attr_set_cacheonly (h, "rank", "0") < 0)
        BAIL_OUT ("flux_attr_set_cacheonly rank failed");
    flux_log_set_redirect (h, diag_logger, NULL);
    flux_log (h, LOG_INFO, "test log message");

    single (h);
    clear_list (logs);

    trio (h);
    clear_list (logs);

    wrongness (h);

    flux_close (h);
    zlist_destroy (&logs);

    done_testing ();
}

/*
 * vi: ts=4 sw=4 expandtab
 */



