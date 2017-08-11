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
#include <stdlib.h>
#include "fiber.h"
#include "iproto_buffer.h"
#include "iobuf.h"

enum {
	CACHED_BUFFERS_MAX = 10,
};
static int cached_buffers_count = 0;
static RLIST_HEAD(cached_buffers);

struct iproto_buffer *
iproto_buffer_new(struct iproto_connection *conn)
{
	struct iproto_buffer *ret;
	if (cached_buffers_count > 0) {
		ret = rlist_shift_entry(&cached_buffers, struct iproto_buffer,
					in_batch);
		--cached_buffers_count;
		goto finish;
	}
	ret = (struct iproto_buffer *) malloc(sizeof(*ret));
	if (ret == NULL) {
		diag_set(OutOfMemory, sizeof(*ret), "malloc", "ret");
		return NULL;
	}
	obuf_create(&ret->obuf, cord_slab_cache(), iobuf_readahead);
finish:
	ret->connection = conn;
	rlist_create(&ret->in_batch);
	return ret;
}

void
iproto_buffer_delete(struct iproto_buffer *buffer)
{
	if (cached_buffers_count < CACHED_BUFFERS_MAX) {
		obuf_reset(&buffer->obuf);
		rlist_add_tail_entry(&cached_buffers, buffer, in_batch);
		++cached_buffers_count;
		return;
	}
	obuf_destroy(&buffer->obuf);
	free(buffer);
}

void
iproto_buffer_delete_list(struct rlist *buffers)
{
	struct iproto_buffer *next, *tmp;
	rlist_foreach_entry_safe(next, buffers, in_batch, tmp)
		iproto_buffer_delete(next);
	rlist_create(buffers);
}
