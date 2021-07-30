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
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <assert.h>
#include <inttypes.h>

#include "message.h"
#include "message_private.h"

static void route_id_destroy (void *data)
{
    if (data) {
        struct route_id *r = data;
        free (r);
    }
}

static struct route_id *route_id_create (const char *id, unsigned int id_len)
{
    struct route_id *r;
    if (!(r = calloc (1, sizeof (*r) + id_len + 1)))
        return NULL;
    if (id && id_len) {
        memcpy (r->id, id, id_len);
        list_node_init (&(r->route_id_node));
    }
    return r;
}

int msg_route_push (flux_msg_t *msg,
                    const char *id,
                    unsigned int id_len)
{
    struct route_id *r;
    if (!(r = route_id_create (id, strlen (id))))
        return -1;
    list_add (&msg->routes, &r->route_id_node);
    msg->routes_len++;
    return 0;
}

int msg_route_append (flux_msg_t *msg,
                      const char *id,
                      unsigned int id_len)
{
    struct route_id *r;
    assert (msg);
    assert ((msg->flags & FLUX_MSGFLAG_ROUTE));
    assert (id);
    if (!(r = route_id_create (id, id_len)))
        return -1;
    list_add_tail (&msg->routes, &r->route_id_node);
    msg->routes_len++;
    return 0;
}

void msg_route_clear (flux_msg_t *msg)
{
    struct route_id *r;
    assert (msg);
    assert ((msg->flags & FLUX_MSGFLAG_ROUTE));
    while ((r = list_pop (&msg->routes, struct route_id, route_id_node)))
        route_id_destroy (r);
    list_head_init (&msg->routes);
    msg->routes_len = 0;
}

int msg_route_delete_last (flux_msg_t *msg)
{
    struct route_id *r;
    assert (msg);
    assert ((msg->flags & FLUX_MSGFLAG_ROUTE));
    if ((r = list_pop (&msg->routes, struct route_id, route_id_node))) {
        route_id_destroy (r);
        msg->routes_len--;
    }
    return 0;
}

static void proto_set_u32 (uint8_t *data, int index, uint32_t val)
{
    uint32_t x = htonl (val);
    int offset = PROTO_OFF_U32_ARRAY + index * 4;
    memcpy (&data[offset], &x, sizeof (x));
}

void msg_proto_setup (const flux_msg_t *msg, uint8_t *data, int len)
{
    assert (len >= PROTO_SIZE);
    assert (msg->type != FLUX_MSGTYPE_ANY);
    memset (data, 0, len);
    data[PROTO_OFF_MAGIC] = PROTO_MAGIC;
    data[PROTO_OFF_VERSION] = PROTO_VERSION;
    data[PROTO_OFF_TYPE] = msg->type;
    data[PROTO_OFF_FLAGS] = msg->flags;
    proto_set_u32 (data, PROTO_IND_USERID, msg->userid);
    proto_set_u32 (data, PROTO_IND_ROLEMASK, msg->rolemask);
    proto_set_u32 (data, PROTO_IND_AUX1, msg->aux1);
    proto_set_u32 (data, PROTO_IND_AUX2, msg->aux2);
}

void proto_get_u32 (const uint8_t *data, int index, uint32_t *val)
{
    uint32_t x;
    int offset = PROTO_OFF_U32_ARRAY + index * 4;
    memcpy (&x, &data[offset], sizeof (x));
    *val = ntohl (x);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

