/************************************************************\
 * Copyright 2024 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _LIBSDEXEC_IOBUF_H
#define _LIBSDEXEC_IOBUF_H

#include <stdbool.h>
#include <sys/types.h>

/* The iobuf container was purpose-built for sdexec/channel.c.
 *
 * Putting data in the buffer:
 * - write up to iobuf_free() bytes to the location returned by iobuf_head()
 * - account for that with iobuf_mark_used().
 *
 * Taking data out of the buffer:
 * - read up to iobuf_used() bytes from the location returned by iobuf_tail()
 * - account for that with iobuf_mark_free().
 *
 * Call iobuf_gc() when done consuming data from the buffer.
 */
struct iobuf *iobuf_create (size_t size);
void iobuf_destroy (struct iobuf *buf);

char *iobuf_head (struct iobuf *buf);
size_t iobuf_free (struct iobuf *buf);
void iobuf_mark_used (struct iobuf *buf, size_t count);

// full in the sense that the entire buffer is used, even after gc
bool iobuf_full (struct iobuf *buf);

char *iobuf_tail (struct iobuf *buf);
size_t iobuf_used (struct iobuf *buf);
void iobuf_mark_free (struct iobuf *buf, size_t count);
void iobuf_gc (struct iobuf *buf);

#endif /* !_LIBSDEXEC_IOBUF_H */

// vi:ts=4 sw=4 expandtab
