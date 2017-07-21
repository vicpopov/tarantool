#ifndef INCLUDES_TARANTOOL_BOX_SEQUENCE_H
#define INCLUDES_TARANTOOL_BOX_SEQUENCE_H
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
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

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** Sequence metadata. */
struct sequence_def {
	/** Sequence id. */
	uint32_t id;
	/** Owner of the sequence. */
	uint32_t uid;
	/**
	 * Id of the space this sequence belongs to or -1
	 * if the sequence does not belong to any space.
	 */
	int32_t space_id;
	/**
	 * The value added to the sequence at each step.
	 * If it is positive, the sequence is ascending,
	 * otherwise it is descending.
	 */
	int64_t step;
	/** Min sequence value. */
	int64_t min;
	/** Max sequence value. */
	int64_t max;
	/** Initial sequence value. */
	int64_t start;
	/**
	 * If this flag is set, the sequence will wrap
	 * upon reaching min or max value by a descending
	 * or ascending sequence respectively.
	 */
	bool cycle;
	/** Sequence name. */
	char name[0];
};

/** Sequence object. */
struct sequence {
	/** Sequence definition. */
	struct sequence_def *def;
	/** Last value returned by the sequence. */
	int64_t value;
	/**
	 * True if the sequence was started, i.e.
	 * (there's a record for it in _sequence_data).
	 */
	bool started;
};

static inline size_t
sequence_def_sizeof(uint32_t name_len)
{
	return sizeof(struct sequence_def) + name_len + 1;
}

/** Reset a sequence. */
static inline void
sequence_reset(struct sequence *seq)
{
	seq->started = false;
}

/** Set a sequence value. */
static inline void
sequence_set(struct sequence *seq, int64_t value)
{
	seq->value = value;
	seq->started = true;
}

/**
 * Get the next sequence value.
 *
 * Return 0 on success. If the sequence isn't cyclic and has
 * reached its limit, return -1 and set diag.
 *
 * Note, this function does not update the sequence value.
 * To advance a sequence, use sequence_set().
 */
int
sequence_get_next(struct sequence *seq, int64_t *result);

/**
 * Find a sequence by id. Return NULL if the sequence was
 * not found.
 */
struct sequence *
sequence_by_id(uint32_t id);

/**
 * A wrapper around sequence_by_id() that sets diag if the
 * sequence was not found in the cache.
 */
struct sequence *
sequence_cache_find(uint32_t id);

/**
 * Insert a new sequence object into the cache or update
 * an existing one if there's already a sequence with
 * the given id in the cache.
 *
 * Return the sequence on success, NULL on memory allocation
 * error.
 */
struct sequence *
sequence_cache_replace(struct sequence_def *def);

/** Delete a sequence from the sequence cache. */
void
sequence_cache_delete(uint32_t id);

/** Initialize the sequence cache. */
void
sequence_cache_init(void);

/** Destroy the sequence cache. */
void
sequence_cache_free(void);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_SEQUENCE_H */
