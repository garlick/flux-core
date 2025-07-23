/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
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
#ifdef HAVE_ARGZ_ADD
#include <argz.h>
#else
#include "src/common/libmissing/argz.h"
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <signal.h>
#include <pthread.h>
#include <uuid.h>
#ifndef UUID_STR_LEN
#define UUID_STR_LEN 37     // defined in later libuuid headers
#endif
#include <flux/core.h>
#include <jansson.h>

#include "src/common/libflux/plugin_private.h"
#include "src/common/libutil/log.h"
#include "src/common/libutil/errprintf.h"
#include "src/common/libutil/errno_safe.h"
#include "src/common/libutil/aux.h"
#include "src/common/libutil/basename.h"
#include "src/common/librouter/subhash.h"
#include "ccan/str/str.h"

#include "module.h"
#include "modservice.h"

struct module_thread {
    pthread_t t;            /* module thread */
    mod_main_f *main;       /* dlopened mod_main() */
    bool mod_main_failed;
    int mod_main_errno;
    flux_t *h;              /* module end of interthread channel */
};

struct module_args {
    int argc;
    char **argv;
    size_t argz_len;
    char *argz;
};

struct broker_module {
    flux_t *h;              /* ref to broker's internal flux_t handle */

    double lastseen;
    flux_t *interthread;    /* broker end of interthread channel */
    flux_watcher_t *interthread_w;
    char uri[128];

    uuid_t uuid;            /* uuid for unique request sender identity */
    char uuid_str[UUID_STR_LEN];
    char *parent_uuid_str;
    flux_msg_t *welcome;

    struct module_thread thd;

    char *name;
    char *path;             /* retain the full path as a key for lookup */
    int status;
    int errnum;
    bool muted;             /* module is under directive 42, no new messages */
    struct aux_item *aux;

    modpoller_cb_f poller_cb;
    void *poller_arg;
    module_status_cb_f status_cb;
    void *status_arg;

    struct disconnect *disconnect;

    struct flux_msglist *deferred_messages;

    struct subhash *sub;
};

static void module_thread_cleanup (void *arg);

static int setup_module_profiling (module_t *p)
{
    size_t len = strlen (p->name);
    // one character longer than target to pass -Wstringop-truncation
    char local_name[17] = {0};
    const char *name_ptr = p->name;
    // pthread name is limited to 16 bytes including \0 on linux
    if (len > 15) {
        strncpy (local_name, p->name, 16);
        local_name[15] = 0;
        name_ptr = local_name;
    }
    // Set the name of each thread to its module name
#if HAVE_PTHREAD_SETNAME_NP_WITH_TID
    (void) pthread_setname_np (pthread_self (), name_ptr);
#else // e.g. macos
    (void) pthread_setname_np (name_ptr);
#endif
    return (0);
}

static int attr_cache_to_json (flux_t *h, json_t **cachep)
{
    json_t *cache;
    const char *name;

    if (!(cache = json_object ()))
        return -1;
    name = flux_attr_cache_first (h);
    while (name) {
        json_t *val = json_string (flux_attr_get (h, name));
        if (!val || json_object_set_new (cache, name, val) < 0) {
            json_decref (val);
            goto error;
        }
        name = flux_attr_cache_next (h);
    }
    *cachep = cache;
    return 0;
error:
    json_decref (cache);
    return -1;
}

static int attr_cache_from_json (flux_t *h, json_t *cache)
{
    const char *name;
    json_t *o;

    json_object_foreach (cache, name, o) {
        const char *val = json_string_value (o);
        if (flux_attr_set_cacheonly (h, name, val) < 0)
            return -1;
    }
    return 0;
}

static void module_args_destroy (struct module_args *ma)
{
    if (ma) {
        int saved_errno = errno;
        free (ma->argv);
        free (ma->argz);
        free (ma);
        errno = saved_errno;
    }
}

static struct module_args *module_args_from_json (json_t *args)
{
    struct module_args *ma;
    size_t index;
    json_t *entry;

    if (!(ma = calloc (1, sizeof (*ma))))
        return NULL;
    json_array_foreach (args, index, entry) {
        const char *s = json_string_value (entry);
        if (s && (argz_add (&ma->argz, &ma->argz_len, s) != 0))
            goto nomem;
    }
    ma->argc = argz_count (ma->argz, ma->argz_len);
    if (!(ma->argv = calloc (1, sizeof (ma->argv[0]) * (ma->argc + 1))))
        goto nomem;
    argz_extract (ma->argz, ma->argz_len, ma->argv);
    return ma;
nomem:
    module_args_destroy (ma);
    errno = ENOMEM;
    return NULL;
}

/* Build a welcome message for a new module that will contain:
 * - the module's name
 * - module arguments (optional)
 * - all immutable broker attributes, for caching (optional)
 * - configuration object
 */
static flux_msg_t *module_welcome_encode (const char *name,
                                          json_t *args,
                                          flux_t *h,
                                          const flux_conf_t *conf)
{
    json_t *obj;
    flux_msg_t *msg = NULL;

    if (!(obj = json_pack ("{s:s}", "name", name)))
        goto nomem;
    if (args) {
        if (json_object_set (obj, "args", args) < 0)
            goto nomem;
    }
    if (h) {
        json_t *o;
        if (attr_cache_to_json (h, &o) < 0)
            goto error;
        if (json_object_set_new (obj, "attrs", o) < 0) {
            json_decref (o);
            goto nomem;
        }
    }
    if (conf) {
        json_t *o;
        if (flux_conf_unpack (conf, NULL, "O", &o) < 0)
            goto error;
        if (json_object_set_new (obj, "conf", o) < 0) {
            json_decref (o);
            goto nomem;
        }
    }
    if (!(msg = flux_request_encode ("welcome", NULL))
        || flux_msg_pack (msg, "O", obj) < 0)
        goto error;
    json_decref (obj);
    return msg;
nomem:
    errno = ENOMEM;
error:
    ERRNO_SAFE_WRAP (json_decref, obj);
    flux_msg_destroy (msg);
    return NULL;
}

/*  Synchronize the FINALIZING state with the broker, so the broker
 *   can stop messages to this module until we're fully shutdown.
 */
static int module_finalizing (module_t *p, double timeout)
{
    flux_future_t *f;

    if (!(f = flux_rpc_pack (p->thd.h,
                             "module.status",
                             FLUX_NODEID_ANY,
                             0,
                             "{s:i}",
                             "status", FLUX_MODSTATE_FINALIZING))
        || flux_future_wait_for (f, timeout) < 0
        || flux_rpc_get (f, NULL)) {
        flux_log_error (p->thd.h, "module.status FINALIZING error");
        flux_future_destroy (f);
        return -1;
    }
    flux_future_destroy (f);
    return 0;
}

static int module_welcome_decode (flux_t *h,
                                  struct module_args **margs,
                                  flux_error_t *error)
{
    struct flux_match match = {
        .typemask = FLUX_MSGTYPE_REQUEST,
        .matchtag = FLUX_MATCHTAG_NONE,
        .topic_glob = "welcome",
    };
    flux_msg_t *msg;
    const char *name;
    json_t *args = NULL;
    json_t *attrs = NULL;
    json_t *conf = NULL;
    struct module_args *ma = NULL;

    if (!(msg = flux_recv (h, match, 0))
        || flux_msg_unpack (msg,
                            "{s:s s?o s?o s?o}",
                            "name", &name,
                            "args", &args,
                            "attrs", &attrs,
                            "conf", &conf) < 0) {
        errprintf (error, "%s", strerror (errno));
        goto error;
    }
    flux_log_set_appname (h, name);
    if (args) {
        if (!(ma = module_args_from_json (args))) {
            errprintf (error,
                       "error parsing module args: %s",
                       strerror (errno));
            goto error;
        }

    }
    if (attrs) {
        if (attr_cache_from_json (h, attrs) < 0) {
            errprintf (error,
                       "error priming broker attribute cache: %s",
                       strerror (errno));
            goto error;
        }
    }
    if (conf) {
        flux_conf_t *cf;
        if (!(cf = flux_conf_pack ("O", conf))
            || flux_set_conf (h, cf) < 0) { // steals conf ref
            errprintf (error,
                       "error setting config object: %s",
                       strerror (errno));
            flux_conf_decref (cf);
            goto error;
        }
    }
    flux_msg_destroy (msg);
    *margs = ma;
    return 0;
error:
    module_args_destroy (ma);
    flux_msg_destroy (msg);
    return -1;
}

static void *module_thread (void *arg)
{
    module_t *p = arg;
    flux_error_t error;
    sigset_t signal_set;
    int errnum;
    char uri[128];
    struct module_args *args = NULL;

    pthread_cleanup_push (module_thread_cleanup, p);

    setup_module_profiling (p);

    /* Connect to broker socket, handle welcome, register built-in services
     */
    if (!(p->thd.h = flux_open (p->uri, 0))) {
        log_err ("flux_open %s", uri);
        goto error;
    }
    if (module_welcome_decode (p->thd.h, &args, &error) < 0) {
        flux_log (p->thd.h, LOG_ERR, "welcome: %s", error.text);
        goto error;
    }
    if (modservice_register (p->thd.h, p) < 0) {
        flux_log_error (p->thd.h, "error registering module services");
        goto error;
    }
    /* Block all signals
     */
    if (sigfillset (&signal_set) < 0) {
        flux_log_error (p->thd.h, "sigfillset");
        goto error;
    }
    if ((errnum = pthread_sigmask (SIG_BLOCK, &signal_set, NULL)) != 0) {
        errno = errnum;
        flux_log_error (p->thd.h, "pthread_sigmask");
        goto error;
    }
    /* Run the module's main().
     */
    if (p->thd.main (p->thd.h,
                     args ? args->argc : 0,
                     args ? args->argv : NULL) < 0)
        goto error;
    module_args_destroy (args);
    goto done;
error:
    p->thd.mod_main_errno = errno;
    p->thd.mod_main_failed = true;
    module_args_destroy (args);
done:
    pthread_cleanup_pop (1);
    return NULL;
}

/* This function is invoked in the module thread context in one of two ways:
 * - module_thread() calls pthread_cleanup_pop(3) upon return of mod_main()
 * - pthread_cancel(3) terminates the module thread at a cancellation point
 * pthread_cancel(3) can be called in two situations:
 * - flux module remove --cancel
 * - when modhash_destroy() is called with lingering modules
 * Since modhash_destroy() is called after exiting the broker reactor loop,
 * the broker won't be responsive to any RPCs from this module thread.
 */
static void module_thread_cleanup (void *arg)
{
    module_t *p = arg;
    flux_msg_t *msg;
    flux_future_t *f;

    if (p->thd.mod_main_failed) {
        if (p->thd.mod_main_errno == 0)
            p->thd.mod_main_errno = ECONNRESET;
        flux_log (p->thd.h, LOG_CRIT, "module exiting abnormally");
    }

    /* Before processing unhandled requests, ensure that this module
     * is "muted" in the broker. This ensures the broker won't try to
     * feed a message to this module after we've closed the handle,
     * which could cause the broker to block.
     */
    if (module_finalizing (p, 1.0) < 0)
        flux_log_error (p->thd.h, "failed to set module state to finalizing");

    /* If any unhandled requests were received during shutdown,
     * respond to them now with ENOSYS.
     */
    while ((msg = flux_recv (p->thd.h, FLUX_MATCH_REQUEST, FLUX_O_NONBLOCK))) {
        const char *topic = "unknown";
        (void)flux_msg_get_topic (msg, &topic);
        flux_log (p->thd.h, LOG_DEBUG, "responding to post-shutdown %s", topic);
        if (flux_respond_error (p->thd.h, msg, ENOSYS, NULL) < 0)
            flux_log_error (p->thd.h, "responding to post-shutdown %s", topic);
        flux_msg_destroy (msg);
    }
    if (!(f = flux_rpc_pack (p->thd.h,
                             "module.status",
                             FLUX_NODEID_ANY,
                             FLUX_RPC_NORESPONSE,
                             "{s:i s:i}",
                             "status", FLUX_MODSTATE_EXITED,
                             "errnum", p->thd.mod_main_errno))) {
        flux_log_error (p->thd.h, "module.status EXITED error");
        goto done;
    }
    flux_future_destroy (f);
done:
    flux_close (p->thd.h);
    p->thd.h = NULL;
}

static void module_cb (flux_reactor_t *r,
                       flux_watcher_t *w,
                       int revents,
                       void *arg)
{
    module_t *p = arg;
    p->lastseen = flux_reactor_now (r);
    if (p->poller_cb)
        p->poller_cb (p, p->poller_arg);
}

module_t *module_create (flux_t *h,
                         const char *parent_uuid,
                         const char *name, // may be NULL
                         const char *path,
                         mod_main_f *mod_main,
                         json_t *args,
                         flux_error_t *error)
{
    flux_reactor_t *r = flux_get_reactor (h);
    module_t *p;

    if (!(p = calloc (1, sizeof (*p))))
        goto nomem;
    p->h = h;
    p->thd.main = mod_main;
    if (!(p->welcome = module_welcome_encode (name,
                                              args,
                                              h,
                                              flux_get_conf (h))))
        goto cleanup;
    if (!(p->parent_uuid_str = strdup (parent_uuid)))
        goto nomem;
    strncpy (p->uuid_str, parent_uuid, sizeof (p->uuid_str) - 1);
    if (!(p->path = strdup (path)))
        goto nomem;
    if (!(p->name = strdup (name)))
        goto nomem;
    if (!(p->sub = subhash_create ())) {
        errprintf (error, "error creating subscription hash");
        goto cleanup;
    }
    uuid_generate (p->uuid);
    uuid_unparse (p->uuid, p->uuid_str);

    /* Broker end of interthread pair is opened here.
     */
    // copying 13 + 37 + 1 = 51 bytes into 128 byte buffer cannot fail
    (void)snprintf (p->uri, sizeof (p->uri), "interthread://%s", p->uuid_str);
    if (!(p->interthread = flux_open (p->uri, FLUX_O_NOREQUEUE))
        || flux_opt_set (p->interthread,
                         FLUX_OPT_ROUTER_NAME,
                         parent_uuid,
                         strlen (parent_uuid) + 1) < 0
        || flux_set_reactor (p->interthread, r) < 0) {
        errprintf (error, "could not create %s interthread handle", p->name);
        goto cleanup;
    }
    if (!(p->interthread_w = flux_handle_watcher_create (r,
                                                         p->interthread,
                                                         FLUX_POLLIN,
                                                         module_cb,
                                                         p))) {
        errprintf (error, "could not create %s flux handle watcher", p->name);
        goto cleanup;
    }
    return p;
nomem:
    errprintf (error, "out of memory");
    errno = ENOMEM;
cleanup:
    module_destroy (p);
    return NULL;
}

const char *module_get_path (module_t *p)
{
    return p && p->path ? p->path : "unknown";
}

const char *module_get_name (module_t *p)
{
    return p && p->name ? p->name : "unknown";
}

const char *module_get_uuid (module_t *p)
{
    return p ? p->uuid_str : "unknown";
}

double module_get_lastseen (module_t *p)
{
    return p ? p->lastseen : 0;
}

int module_get_status (module_t *p)
{
    return p ? p->status : 0;
}

void *module_aux_get (module_t *p, const char *name)
{
    if (!p) {
        errno = EINVAL;
        return NULL;
    }
    return aux_get (p->aux, name);
}

int module_aux_set (module_t *p,
                    const char *name,
                    void *val,
                    flux_free_f destroy)
{
    if (!p) {
        errno = EINVAL;
        return -1;
    }
    return aux_set (&p->aux, name, val, destroy);
}

flux_msg_t *module_recvmsg (module_t *p)
{
    flux_msg_t *msg;
    msg = flux_recv (p->interthread, FLUX_MATCH_ANY, FLUX_O_NONBLOCK);
    return msg;
}

int module_sendmsg_new (module_t *p, flux_msg_t **msg)
{
    int type;
    const char *topic;

    if (!msg || !*msg)
        return 0;
    if (flux_msg_get_type (*msg, &type) < 0
        || flux_msg_get_topic (*msg, &topic) < 0)
        return -1;
    /* Muted modules only accept response to module.status
     */
    if (p->muted) {
        if (type != FLUX_MSGTYPE_RESPONSE
            || !streq (topic, "module.status")) {
            errno = ENOSYS;
            return -1;
        }
    }
    if (p->deferred_messages) {
        if (flux_msglist_append (p->deferred_messages, *msg) < 0)
            return -1;
        flux_msg_decref (*msg);
        *msg = NULL;
        return 0;
    }
    return flux_send_new (p->interthread, msg, 0);
}

int module_disconnect_arm (module_t *p,
                           const flux_msg_t *msg,
                           disconnect_send_f cb,
                           void *arg)
{
    if (!p->disconnect) {
        if (!(p->disconnect = disconnect_create (cb, arg)))
            return -1;
    }
    if (disconnect_arm (p->disconnect, msg) < 0)
        return -1;
    return 0;
}

void module_destroy (module_t *p)
{
    int e;
    void *res;
    int saved_errno = errno;

    if (!p)
        return;

    if (p->thd.t) {
        if ((e = pthread_join (p->thd.t, &res)) != 0)
            log_errn_exit (e, "pthread_join");
        if (p->status != FLUX_MODSTATE_EXITED) {
            /* Calls broker.c module_status_cb() => service_remove_byuuid()
             * and releases a reference on 'p'.  Without this, disconnect
             * requests sent when other modules are destroyed can still find
             * this service name and trigger a use-after-free segfault.
             * See also: flux-framework/flux-core#4564.
             */
            module_set_status (p, FLUX_MODSTATE_EXITED);
        }
        if (res == PTHREAD_CANCELED)
            flux_log (p->h, LOG_DEBUG, "%s thread was canceled", p->name);
    }

    /* Send disconnect messages to services used by this module.
     */
    disconnect_destroy (p->disconnect);

    flux_watcher_destroy (p->interthread_w);
    flux_close (p->interthread);

    free (p->name);
    free (p->path);
    free (p->parent_uuid_str);
    flux_msg_decref (p->welcome);
    flux_msglist_destroy (p->deferred_messages);
    subhash_destroy (p->sub);
    aux_destroy (&p->aux);
    free (p);
    errno = saved_errno;
}

/* Send shutdown request, broker to module.
 */
int module_stop (module_t *p, flux_t *h)
{
    char *topic = NULL;
    flux_future_t *f = NULL;
    int rc = -1;

    if (asprintf (&topic, "%s.shutdown", p->name) < 0)
        goto done;
    if (!(f = flux_rpc (h,
                        topic,
                        NULL,
                        FLUX_NODEID_ANY,
                        FLUX_RPC_NORESPONSE)))
        goto done;
    rc = 0;
done:
    free (topic);
    flux_future_destroy (f);
    return rc;
}

void module_mute (module_t *p)
{
    p->muted = true;
}

int module_set_defer (module_t *p, bool flag)
{
    if (flag && !p->deferred_messages) {
        if (!(p->deferred_messages = flux_msglist_create ()))
            return -1;
    }
    if (!flag && p->deferred_messages) {
        const flux_msg_t *msg;
        while ((msg = flux_msglist_pop (p->deferred_messages))) {
            if (flux_send_new (p->interthread, (flux_msg_t **)&msg, 0) < 0) {
                flux_msg_decref (msg);
                return -1;
            }
        }
        flux_msglist_destroy (p->deferred_messages);
        p->deferred_messages = NULL;
    }
    return 0;
}

int module_start (module_t *p)
{
    int errnum;
    int rc = -1;

    flux_watcher_start (p->interthread_w);
    if ((errnum = pthread_create (&p->thd.t, NULL, module_thread, p))) {
        errno = errnum;
        goto done;
    }
    if (flux_send (p->interthread, p->welcome, 0) < 0)
        goto done;
    rc = 0;
done:
    return rc;
}

int module_cancel (module_t *p, flux_error_t *error)
{
    if (p->thd.t) {
        int e;
        if ((e = pthread_cancel (p->thd.t)) != 0 && e != ESRCH) {
            errprintf (error, "pthread_cancel: %s", strerror (e));
            return -1;
        }
    }
    return 0;
}

void module_set_poller_cb (module_t *p, modpoller_cb_f cb, void *arg)
{
    p->poller_cb = cb;
    p->poller_arg = arg;
}

void module_set_status_cb (module_t *p, module_status_cb_f cb, void *arg)
{
    p->status_cb = cb;
    p->status_arg = arg;
}

void module_set_status (module_t *p, int new_status)
{
    if (new_status == FLUX_MODSTATE_INIT || p->status == FLUX_MODSTATE_EXITED)
        return; // illegal state transitions
    int prev_status = p->status;
    p->status = new_status;
    if (p->status_cb)
        p->status_cb (p, prev_status, p->status_arg);
}

void module_set_errnum (module_t *p, int errnum)
{
    p->errnum = errnum;
}

int module_get_errnum (module_t *p)
{
    return p->errnum;
}

int module_subscribe (module_t *p, const char *topic)
{
    return subhash_subscribe (p->sub, topic);
}

int module_unsubscribe (module_t *p, const char *topic)
{
    return subhash_unsubscribe (p->sub, topic);
}

int module_event_cast (module_t *p, const flux_msg_t *msg)
{
    const char *topic;

    if (flux_msg_get_topic (msg, &topic) < 0)
        return -1;
    if (subhash_topic_match (p->sub, topic)) {
        flux_msg_t *cpy;
        if (!(cpy = flux_msg_copy (msg, true))
            || module_sendmsg_new (p, &cpy) < 0) {
            flux_msg_decref (cpy);
            return -1;
        }
    }
    return 0;
}

ssize_t module_get_send_queue_count (module_t *p)
{
    size_t count;
    if (flux_opt_get (p->interthread,
                      FLUX_OPT_SEND_QUEUE_COUNT,
                      &count,
                      sizeof (count)) < 0)
        return -1;
    return count;
}

ssize_t module_get_recv_queue_count (module_t *p)
{
    size_t count;
    if (flux_opt_get (p->interthread,
                      FLUX_OPT_RECV_QUEUE_COUNT,
                      &count,
                      sizeof (count)) < 0)
        return -1;
    return count;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
