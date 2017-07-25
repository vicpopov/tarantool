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

#include "rtmalloc.h"
#include "fiber.h"
#include "diag.h"

#ifdef HAVE_MALLOC_QUOTA

extern struct quota runtime_quota;

void *
rtcalloc(size_t count, size_t size)
{
	void *ret = calloc(count, size);
	if (ret == NULL)
		goto error;
	size_t real_size = malloc_usable_size(ret);
	if (quota_lease(&cord()->runtime_quota, real_size) == 0)
		return ret;
	free(ret);
error:
	diag_set(OutOfMemory, size * count, "calloc", "ret");
	return NULL;
}

void
rtfree(void *ptr)
{
	size_t real_size = malloc_usable_size(ptr);
	quota_end_lease(&cord()->runtime_quota, real_size);
	free(ptr);
}

void *
rtmalloc(size_t size)
{
	void *ret = malloc(size);
	if (ret == NULL)
		goto error;
	size_t real_size = malloc_usable_size(ret);
	if (quota_lease(&cord()->runtime_quota, real_size) == 0)
		return ret;
	free(ret);
error:
	diag_set(OutOfMemory, size, "malloc", "ret");
	return NULL;
}

void *
rtrealloc(void *ptr, size_t size)
{
	struct quota_lessor *q = &cord()->runtime_quota;
	size_t old_size = malloc_usable_size(ptr);
	if (old_size >= size)
		return ptr;
	void *new_ptr = malloc(size);
	if (new_ptr == NULL)
		goto error;
	size_t new_size = malloc_usable_size(new_ptr);
	assert(new_size >= old_size);
	memcpy(new_ptr, ptr, old_size);
	if (quota_lease(q, new_size - old_size) == 0) {
		free(ptr);
		return new_ptr;
	}
	free(new_ptr);
error:
	diag_set(OutOfMemory, size, "realloc", "ptr");
	return NULL;
}

#endif /* HAVE_MALLOC_QUOTA */
