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
#include "sequence.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "assoc.h"
#include "diag.h"
#include "errcode.h"
#include "say.h"
#include "trivia/util.h"

static struct mh_i32ptr_t *sequence_cache;

struct sequence *
sequence_by_id(uint32_t id)
{
	mh_int_t k = mh_i32ptr_find(sequence_cache, id, NULL);
	return (k == mh_end(sequence_cache) ? NULL :
		mh_i32ptr_node(sequence_cache, k)->val);
}

struct sequence *
sequence_cache_find(uint32_t id)
{
	struct sequence *seq = sequence_by_id(id);
	if (seq == NULL)
		diag_set(ClientError, ER_NO_SUCH_SEQUENCE, int2str(id));
	return seq;
}

struct sequence *
sequence_cache_replace(struct sequence_def *def)
{
	struct sequence *seq = sequence_by_id(def->id);
	if (seq == NULL) {
		/* Create a new sequence. */
		seq = calloc(1, sizeof(*seq));
		if (seq == NULL) {
			diag_set(OutOfMemory, sizeof(*seq),
				 "malloc", "sequence");
			return NULL;
		}
		struct mh_i32ptr_node_t node = { def->id, seq };
		mh_i32ptr_put(sequence_cache, &node, NULL, NULL);
	} else {
		/* Update an existing sequence. */
		free(seq->def);
	}
	seq->def = def;
	return seq;
}

void
sequence_cache_delete(uint32_t id)
{
	struct sequence *seq = sequence_by_id(id);
	if (seq != NULL) {
		mh_i32ptr_del(sequence_cache, seq->def->id, NULL);
		free(seq->def);
		TRASH(seq);
		free(seq);
	}
}

void
sequence_cache_init(void)
{
	sequence_cache = mh_i32ptr_new();
	if (sequence_cache == NULL)
		panic("failed to allocate sequence cache");
}

void
sequence_cache_free(void)
{
	mh_i32ptr_delete(sequence_cache);
}

int
sequence_get_next(struct sequence *seq, int64_t *result)
{
	int64_t value;
	struct sequence_def *def = seq->def;
	if (!seq->started) {
		value = def->start;
		goto done;
	}
	value = seq->value;
	if (def->step > 0) {
		if (value < def->min) {
			value = def->min;
			goto done;
		}
		if (value >= 0 && def->step > INT64_MAX - value)
			goto overflow;
		value += def->step;
		if (value > def->max)
			goto overflow;
	} else {
		assert(def->step < 0);
		if (value > def->max) {
			value = def->max;
			goto done;
		}
		if (value < 0 && def->step < INT64_MIN - value)
			goto overflow;
		value += def->step;
		if (value < def->min)
			goto overflow;
	}
done:
	assert(value >= def->min && value <= def->max);
	*result = value;
	return 0;
overflow:
	if (!def->cycle) {
		diag_set(ClientError, ER_SEQUENCE_OVERFLOW, def->name);
		return -1;
	}
	value = def->step > 0 ? def->min : def->max;
	goto done;
}
