/************************************************************\
 * Copyright 2021 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _FLUX_CORE_MESSAGE_PRIVATE_H
#define _FLUX_CORE_MESSAGE_PRIVATE_H

#include <stdint.h>
#include <jansson.h>

#include "src/common/libutil/errno_safe.h"
/* czmq and ccan both define streq */
#ifdef streq
#undef streq
#endif
#include "src/common/libccan/ccan/list/list.h"

#include "message.h"

/* PROTO consists of 4 byte prelude followed by a fixed length
 * array of u32's in network byte order.
 */
#define PROTO_MAGIC         0x8e
#define PROTO_VERSION       1

#define PROTO_OFF_MAGIC     0 /* 1 byte */
#define PROTO_OFF_VERSION   1 /* 1 byte */
#define PROTO_OFF_TYPE      2 /* 1 byte */
#define PROTO_OFF_FLAGS     3 /* 1 byte */
#define PROTO_OFF_U32_ARRAY 4

/* aux1
 *
 * request - nodeid
 * response - errnum
 * event - sequence
 * keepalive - errnum
 *
 * aux2
 *
 * request - matchtag
 * response - matchtag
 * event - not used
 * keepalive - status
 */
#define PROTO_IND_USERID    0
#define PROTO_IND_ROLEMASK  1
#define PROTO_IND_AUX1      2
#define PROTO_IND_AUX2      3

#define PROTO_U32_COUNT     4
#define PROTO_SIZE          4 + (PROTO_U32_COUNT * 4)

#define IOVECINCR           4

struct flux_msg {
    // optional route list, if FLUX_MSGFLAG_ROUTE
    struct list_head routes;
    int routes_len;     /* to avoid looping */

    // optional topic frame, if FLUX_MSGFLAG_TOPIC
    char *topic;

    // optional payload frame, if FLUX_MSGFLAG_PAYLOAD
    void *payload;
    size_t payload_size;

    // required proto frame data
    uint8_t type;
    uint8_t flags;
    uint32_t userid;
    uint32_t rolemask;
    union {
        uint32_t nodeid;  // request
        uint32_t sequence; // event
        uint32_t errnum; // response, keepalive
        uint32_t aux1; // common accessor
    };
    union {
        uint32_t matchtag; // request, response
        uint32_t status; // keepalive
        uint32_t aux2; // common accessor
    };

    json_t *json;
    char *lasterr;
    struct aux_item *aux;
    int refcount;
};

struct route_id {
    struct list_node route_id_node;
    char id[0];                 /* variable length id stored at end of struct */
};

/* 'transport_data' is for any auxiliary transport data user may wish
 * to associate with iovec, user is responsible to free/destroy the
 * field
 */
struct msg_iovec {
    const void *data;
    size_t size;
    void *transport_data;
};

void msg_proto_setup (const flux_msg_t *msg, uint8_t *data, int len);

int msg_route_push (flux_msg_t *msg,
                    const char *id,
                    unsigned int id_len);

int msg_route_append (flux_msg_t *msg,
                      const char *id,
                      unsigned int id_len);

void msg_route_clear (flux_msg_t *msg);

int msg_route_delete_last (flux_msg_t *msg);

int iovec_to_msg (flux_msg_t *msg,
                  struct msg_iovec *iov,
                  int iovcnt);

int msg_to_iovec (const flux_msg_t *msg,
                  uint8_t *proto,
                  int proto_len,
                  struct msg_iovec **iovp,
                  int *iovcntp);

#endif /* !_FLUX_CORE_MESSAGE_PRIVATE_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

