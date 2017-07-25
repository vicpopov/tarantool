#ifndef TARANTOOL_RTMALLOC_H_INCLUDED
#define TARANTOOL_RTMALLOC_H_INCLUDED
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

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#include "trivia/config.h"
#include "diag.h"

#ifdef HAVE_MALLOC_IN_DIR
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

/**
 * Rtmalloc - RunTimeMalloc - malloc-based allocator, which
 * sets diagnostics area on allocation errors and can account
 * malloc in a runtime quota.
 */

#if !defined(HAVE_MALLOC_USABLE_SIZE) && defined(HAVE_MALLOC_SIZE)
#define malloc_usable_size malloc_size
#endif

/**
 * If malloc can not use runtime quota, then all malloc wrappers
 * only sets diagnostics area on errors.
 */
#ifndef HAVE_MALLOC_QUOTA

static inline void *
rtcalloc(size_t count, size_t size)
{
	void *ret = calloc(count, size);
	if (ret == NULL)
		diag_set(OutOfMemory, size * count, "calloc", "ret");
	return ret;
}

static inline void
rtfree(void *ptr)
{
	free(ptr);
}

static inline void *
rtmalloc(size_t size)
{
	void *ret = malloc(size);
	if (ret == NULL)
		diag_set(OutOfMemory, size, "malloc", "ret");
	return ret;
}

static inline void *
rtrealloc(void *ptr, size_t size)
{
	ptr = realloc(ptr, size);
	if (ptr == NULL)
		diag_set(OutOfMemory, size, "realloc", "ptr");
	return ptr;
}

/**
 * If the malloc quota is enabled, then RunTimeMalloc used
 * runtime quota (@sa memory.c).
 */
#else /* HAVE_MALLOC_QUOTA */

void *
rtcalloc(size_t count, size_t size);

void
rtfree(void *ptr);

void *
rtmalloc(size_t size);

void *
rtrealloc(void *ptr, size_t size);

#endif /* HAVE_MALLOC_QUOTA */

#if defined(__cplusplus)
} /* extern "C" */

/**
 * C++ RunTimeMalloc wrappers, which can throw exceptions.
 */
static inline void *
rtcalloc_xc(size_t count, size_t size)
{
	void *ret = rtcalloc(count, size);
	if (ret == NULL)
		diag_raise();
	return ret;
}

static inline void *
rtmalloc_xc(size_t size)
{
	void *ret = rtmalloc(size);
	if (ret == NULL)
		diag_raise();
	return ret;
}

static inline void *
rtrealloc_xc(void *ptr, size_t size)
{
	void *ret = rtrealloc(ptr, size);
	if (ret == NULL)
		diag_raise();
	return ret;
}

#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_RTMALLOC_H_INCLUDED */
