#ifndef TARANTOOL_IPROTO_BUFFER_H_INCLUDED
#define TARANTOOL_IPROTO_BUFFER_H_INCLUDED
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "small/obuf.h"
#include "small/rlist.h"

#ifdef __cplusplus
extern "C" {
#endif

struct iproto_connection;

/**
 * Output buffer used by tx to write responses for one connection
 * and by iproto to flush data. Lifecycle:
 *
 *     global buffers_to_iproto        tx thread
 *               v
 *   iproto_msg.buffers_to_flush       tx thread
 *               v
 * iproto_connection.buffers_to_flush  iproto thread
 *               v
 *      global buffers_to_tx           iproto thread
 *               v
 *   iproto_msg.flushed_buffers        iproto thread
 *               v
 *          utilization                tx thread
 */
struct iproto_buffer {
	/** Response data. */
	struct obuf obuf;
	/** Owner connection. Flush to its socket. */
	struct iproto_connection *connection;
	/**
	 * Member of either buffers_to_iproto list
	 * or iproto_msg.flushed_buffers
	 * or iproto_msg.buffers_to_flush
	 * or iproto_connection.buffers_to_flush
	 * or buffers_to_tx list
	 * or cached_buffers list.
	 */
	struct rlist in_batch;
};

/**
 * Create a new iproto_buffer associated with a specified
 * connection. Buffer is associated with a connection to be wrote
 * in its socket further.
 * @param conn Connection to associate with.
 *
 * @retval not NULL Created iproto_buffer.
 * @retval     NULL Memory error.
 */
struct iproto_buffer *
iproto_buffer_new(struct iproto_connection *conn);

/**
 * Delete iproto_buffer, destroy output buffer with response.
 * @param buffer Buffer to delete.
 */
void
iproto_buffer_delete(struct iproto_buffer *buffer);

/**
 * Delete all buffers in a list and make it empty.
 * @param buffers List of iproto_buffer objects.
 */
void
iproto_buffer_delete_list(struct rlist *buffers);

#ifdef __cplusplus
}
#endif

#endif /* TARANTOOL_IPROTO_BUFFER_H_INCLUDED */
