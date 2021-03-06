#ifndef TARANTOOL_BOX_SPACE_H_INCLUDED
#define TARANTOOL_BOX_SPACE_H_INCLUDED
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
#include "key_def.h"
#include "small/rlist.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct Index;
struct Handler;

struct space {
	struct access access[BOX_USER_MAX];
	/**
	 * Reflects the current space state and is also a vtab
	 * with methods. Unlike a C++ vtab, changes during space
	 * life cycle, throughout phases of recovery or with
	 * deletion and addition of indexes.
	 */
	struct Handler *handler;

	/** Triggers fired after space_replace() -- see txn_commit_stmt(). */
	struct rlist on_replace;
	/** Triggers fired before space statement */
	struct rlist on_stmt_begin;
	/* Number of bytes used in memory by tuples in the space. */
	size_t bsize;
	/**
	 * The number of *enabled* indexes in the space.
	 *
	 * After all indexes are built, it is equal to the number
	 * of non-nil members of the index[] array.
	 */
	uint32_t index_count;
	/**
	 * There may be gaps index ids, i.e. index 0 and 2 may exist,
	 * while index 1 is not defined. This member stores the
	 * max id of a defined index in the space. It defines the
	 * size of index_map array.
	 */
	uint32_t index_id_max;
	/** Space meta. */
	struct space_def *def;
	/**
	 * Number of times the space has been truncated.
	 * Updating this counter via _truncate space triggers
	 * space truncation.
	 */
	uint64_t truncate_count;
	/** Enable/disable triggers. */
	bool run_triggers;

	/** Default tuple format used by this space */
	struct tuple_format *format;
	/**
	 * Sparse array of indexes defined on the space, indexed
	 * by id. Used to quickly find index by id (for SELECTs).
	 */
	struct Index **index_map;
	/**
	 * Dense array of indexes defined on the space, in order
	 * of index id. Initially stores only the primary key at
	 * position 0, and is fully built by
	 * space_build_secondary_keys().
	 */
	struct Index *index[];
};

/** Get space ordinal number. */
static inline uint32_t
space_id(struct space *space) { return space->def->id; }

/** Get space name. */
static inline const char *
space_name(const struct space *space)
{
	return space->def->name;
}

/** Return true if space is temporary. */
static inline bool
space_is_temporary(struct space *space) { return space->def->opts.temporary; }

void
space_run_triggers(struct space *space, bool yesno);

/**
 * Get index by index id.
 * @return NULL if the index is not found.
 */
static inline struct Index *
space_index(struct space *space, uint32_t id)
{
	if (id <= space->index_id_max)
		return space->index_map[id];
	return NULL;
}

/**
 * Look up the index by id.
 */
static inline struct Index *
index_find(struct space *space, uint32_t index_id)
{
	struct Index *index = space_index(space, index_id);
	if (index == NULL) {
		diag_set(ClientError, ER_NO_SUCH_INDEX, index_id,
			 space_name(space));
		error_log(diag_last_error(diag_get()));
	}
	return index;
}

/**
 * Returns number of bytes used in memory by tuples in the space.
 */
size_t
space_bsize(struct space *space);

/** Get definition of the n-th index of the space. */
struct index_def *
space_index_def(struct space *space, int n);

/**
 * Get name of the index by its identifier and parent space.
 *
 * @param space Parent space.
 * @param id    Index identifier.
 *
 * @retval not NULL Index name.
 * @retval     NULL No index with the specified identifier.
 */
const char *
index_name_by_id(struct space *space, uint32_t id);

#if defined(__cplusplus)
} /* extern "C" */

#include "index.h"
#include "engine.h"

/** Check whether or not the current user can be granted
 * the requested access to the space.
 */
void
access_check_space(struct space *space, uint8_t access);

static inline bool
space_is_memtx(struct space *space) { return space->handler->engine->id == 0; }

/** Return true if space is run under vinyl engine. */
static inline bool
space_is_vinyl(struct space *space) { return strcmp(space->handler->engine->name, "vinyl") == 0; }

void space_noop(struct space *space);

uint32_t
space_size(struct space *space);

/**
 * Allocate and initialize a space. The space
 * needs to be loaded before it can be used
 * (see space->handler->recover()).
 * @param space_def Space definition.
 * @param key_list List of index_defs.
 *
 * @retval Space object.
 */
struct space *
space_new(struct space_def *space_def, struct rlist *key_list);

/** Destroy and free a space. */
void
space_delete(struct space *space);

/**
 * Dump space definition (key definitions, key count)
 * for ALTER.
 */
void
space_dump_def(const struct space *space, struct rlist *key_list);

/**
 * Exchange two index objects in two spaces. Used
 * to update a space with a newly built index, while
 * making sure the old index doesn't leak.
 */
void
space_swap_index(struct space *lhs, struct space *rhs,
		 uint32_t lhs_id, uint32_t rhs_id);

/** Rebuild index map in a space after a series of swap index. */
void
space_fill_index_map(struct space *space);

/**
 * Look up the index by id, and throw an exception if not found.
 */
static inline struct Index *
index_find_xc(struct space *space, uint32_t index_id)
{
	struct Index *index = index_find(space, index_id);
	if (index == NULL)
		diag_raise();
	return index;
}

static inline struct Index *
index_find_unique(struct space *space, uint32_t index_id)
{
	struct Index *index = index_find_xc(space, index_id);
	if (! index->index_def->opts.is_unique)
		tnt_raise(ClientError, ER_MORE_THAN_ONE_TUPLE);
	return index;
}

class MemtxIndex;

/**
 * Find an index in a system space. Throw an error
 * if we somehow deal with a non-memtx space (it can't
 * be used for system spaces.
 */
static inline MemtxIndex *
index_find_system(struct space *space, uint32_t index_id)
{
	if (! space_is_memtx(space)) {
		tnt_raise(ClientError, ER_UNSUPPORTED,
			  space->handler->engine->name, "system data");
	}
	return (MemtxIndex *) index_find_xc(space, index_id);
}

/**
 * Updates space bsize field: decrease it by old tuple's bsize and
 * increase it by new one's.
 * Return an increment in space.bsize
 */
ptrdiff_t
space_bsize_update(struct space *space,
		   const struct tuple *old_tuple,
		   const struct tuple *new_tuple);

/**
 * Revert previous change in space_bsize
 */
void
space_bsize_rollback(struct space *space, ptrdiff_t bsize_change);

#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_SPACE_H_INCLUDED */
