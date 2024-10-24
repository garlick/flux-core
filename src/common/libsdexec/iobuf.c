/************************************************************\
 * Copyright 2024 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* iobuf.c - output buffer for channel.c
 *
 * iobuf is a linear buffer which allows data to be removed in contiguous
 * chunks of our choosing (for example lines) without copying, unlike cbuf.
 * However, the buffer space has to be reclaimed after data has been taken
 * out by calling output_gc().
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "iobuf.h"

struct iobuf {
    char *data;
    size_t size;
    size_t offset;  // valid data begins at buf.data + buf.offset
    size_t used;    // bytes used starting at buf.data + buf.offset
};

char *iobuf_head (struct iobuf *buf)
{
    return buf->data + buf->offset + buf->used;
}

size_t iobuf_free (struct iobuf *buf)
{
    return buf->size - (buf->offset + buf->used);
}

void iobuf_mark_used (struct iobuf *buf, size_t count)
{
    buf->used += count;
}

// "full" in the sense that even after gc there will be no room for new data
bool iobuf_full (struct iobuf *buf)
{
    return (buf->size == buf->used) ? true : false;
}

char *iobuf_tail (struct iobuf *buf)
{
    return buf->data + buf->offset;
}

size_t iobuf_used (struct iobuf *buf)
{
    return buf->used;
}

void iobuf_mark_free (struct iobuf *buf, size_t count)
{
    buf->offset += count;
    buf->used -= count;
}

void iobuf_gc (struct iobuf *buf)
{
    if (buf->offset > 0) {
        memmove (buf->data, buf->data + buf->offset, buf->used);
        buf->offset = 0;
    }
}

struct iobuf *iobuf_create (size_t size)
{
    struct iobuf *buf;
    if (!(buf = calloc (1, sizeof (*buf) + size)))
        return NULL;
    buf->data = (char *)(buf + 1);
    buf->size = size;
    return buf;
}

void iobuf_destroy (struct iobuf *buf)
{
    if (buf) {
        int saved_errno = errno;
        free (buf);
        errno = saved_errno;
    }
}

// vi:ts=4 sw=4 expandtab
