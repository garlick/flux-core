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

#endif /* !_FLUX_CORE_MESSAGE_PRIVATE_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

