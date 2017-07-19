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
#include "index.h"
#include "tuple.h"
#include "say.h"
#include "schema.h"
#include "user_def.h"
#include "space.h"
#include "iproto_constants.h"
#include "txn.h"
#include "rmean.h"
#include "info.h"
#include "scoped_guard.h"

/* {{{ Utilities. **********************************************/

UnsupportedIndexFeature::UnsupportedIndexFeature(const char *file,
	unsigned line, const Index *index, const char *what)
	: ClientError(file, line, ER_UNKNOWN)
{
	struct index_def *index_def = index->index_def;
	struct space *space = space_cache_find(index_def->space_id);
	m_errcode = ER_UNSUPPORTED_INDEX_FEATURE;
	error_format_msg(this, tnt_errcode_desc(m_errcode), index_def->name,
			 index_type_strs[index_def->type],
			 space->def->name, space->def->engine_name, what);
}

int
key_validate(struct index_def *index_def, enum iterator_type type, const char *key,
	     uint32_t part_count)
{
	assert(key != NULL || part_count == 0);
	if (part_count == 0) {
		/*
		 * Zero key parts are allowed:
		 * - for TREE index, all iterator types,
		 * - ITER_ALL iterator type, all index types
		 * - ITER_GT iterator in HASH index (legacy)
		 */
		if (index_def->type == TREE || type == ITER_ALL ||
		    (index_def->type == HASH && type == ITER_GT))
			return 0;
		/* Fall through. */
	}

	if (index_def->type == RTREE) {
		unsigned d = index_def->opts.dimension;
		if (part_count != 1 && part_count != d && part_count != d * 2) {
			diag_set(ClientError, ER_KEY_PART_COUNT, d  * 2,
				 part_count);
			return -1;
		}
		if (part_count == 1) {
			enum mp_type mp_type = mp_typeof(*key);
			if (key_mp_type_validate(FIELD_TYPE_ARRAY, mp_type,
						 ER_KEY_PART_TYPE, 0))
				return -1;
			uint32_t array_size = mp_decode_array(&key);
			if (array_size != d && array_size != d * 2) {
				diag_set(ClientError, ER_RTREE_RECT, "Key", d,
					 d * 2);
				return -1;
			}
			for (uint32_t part = 0; part < array_size; part++) {
				enum mp_type mp_type = mp_typeof(*key);
				mp_next(&key);
				if (key_mp_type_validate(FIELD_TYPE_NUMBER,
							 mp_type,
							 ER_KEY_PART_TYPE, 0))
					return -1;
			}
		} else {
			for (uint32_t part = 0; part < part_count; part++) {
				enum mp_type mp_type = mp_typeof(*key);
				mp_next(&key);
				if (key_mp_type_validate(FIELD_TYPE_NUMBER,
							 mp_type,
							 ER_KEY_PART_TYPE,
							 part))
					return -1;
			}
		}
	} else {
		if (part_count > index_def->key_def.part_count) {
			diag_set(ClientError, ER_KEY_PART_COUNT,
				 index_def->key_def.part_count, part_count);
			return -1;
		}

		/* Partial keys are allowed only for TREE index type. */
		if (index_def->type != TREE && part_count < index_def->key_def.part_count) {
			diag_set(ClientError, ER_PARTIAL_KEY,
				 index_type_strs[index_def->type],
				 index_def->key_def.part_count,
				 part_count);
			return -1;
		}
		if (key_validate_parts(&index_def->key_def, key,
				       part_count) != 0)
			return -1;
	}
	return 0;
}

int
primary_key_validate(struct key_def *key_def, const char *key,
		     uint32_t part_count)
{
	assert(key != NULL || part_count == 0);
	if (key_def->part_count != part_count) {
		diag_set(ClientError, ER_EXACT_MATCH, key_def->part_count,
			 part_count);
		return -1;
	}
	return key_validate_parts(key_def, key, part_count);
}

/**
 * Create a space format from a list of index_defs.
 * Since secondary indexes can internally create their own key_defs
 * by merging original key_def with a key_def of the primary index,
 * the appropriate space format must consider this behaviour and create
 * field offsets for merged key_defs' fields.
 * @param vtab - virtual table of desired format.
 * @param key_list - a list of index_defs @sa space_new
 * @return - new unreferenced format (throws on error)
 */
struct tuple_format *
create_space_format(struct tuple_format_vtab *vtab, struct rlist *key_list)
{
	uint16_t key_count = 0;
	struct index_def *index_def;
	struct index_def *pk = NULL;
	rlist_foreach_entry(index_def, key_list, link) {
		key_count++;
		/* Find the primary key, we need to create it first. */
		if (index_def->iid == 0)
			pk = index_def;
	}
	/* A space with a secondary key without a primary is illegal. */
	assert(key_count == 0 || pk != NULL);
	struct key_def **keys = (struct key_def **)
		region_alloc_xc(&fiber()->gc, sizeof(*keys) * key_count);
	uint32_t key_no = 0;
	rlist_foreach_entry(index_def, key_list, link) {
		if (index_def->type == TREE && !index_def->opts.is_unique) {
			/*
			 * Tree index will use its own key_def different
			 * from index_def->key_def. We have to create similar
			 * temporary key_def and pass it to tuple_format_new
			 * in order to create proper field map.
			 */
			struct key_def *tree_key_def =
				key_def_merge(&index_def->key_def,
					      &pk->key_def);
			auto tree_key_def_guard = make_scoped_guard(
				[&]() { box_key_def_delete(tree_key_def); });
			size_t key_def_size =
				key_def_sizeof(tree_key_def->part_count);
			struct key_def *key_def = (struct key_def *)
				region_alloc_xc(&fiber()->gc, key_def_size);
			memcpy(key_def, tree_key_def, key_def_size);
			keys[key_no++] = key_def;
		} else {
			keys[key_no++] = &index_def->key_def;
		}
	}
	assert(key_no == key_count);
	struct tuple_format *fmt = tuple_format_new(vtab, keys, key_count, 0);
	if (fmt == NULL)
		diag_raise();
	return fmt;
}

char *
box_tuple_extract_key(const box_tuple_t *tuple, uint32_t space_id,
	uint32_t index_id, uint32_t *key_size)
{
	try {
		struct space *space = space_by_id(space_id);
		Index *index = index_find_xc(space, index_id);
		return tuple_extract_key(tuple, &index->index_def->key_def,
					 key_size);
	} catch (ClientError *e) {
		return NULL;
	}
}

/* }}} */

/* {{{ Index -- base class for all indexes. ********************/

Index::Index(struct index_def *index_def_arg)
	:index_def(NULL), schema_version(::schema_version)
{
	index_def = index_def_dup(index_def_arg);
	if (index_def == NULL)
		diag_raise();
}

Index::~Index()
{
	index_def_delete(index_def);
}

void
Index::commitCreate()
{
}

void
Index::commitDrop()
{
}

size_t
Index::size() const
{
	tnt_raise(UnsupportedIndexFeature, this, "size()");
	return 0;
}

struct tuple *
Index::min(const char* /* key */, uint32_t /* part_count */) const
{
	tnt_raise(UnsupportedIndexFeature, this, "min()");
	return NULL;
}

struct tuple *
Index::max(const char* /* key */, uint32_t /* part_count */) const
{
	tnt_raise(UnsupportedIndexFeature, this, "max()");
	return NULL;
}

struct tuple *
Index::random(uint32_t rnd) const
{
	(void) rnd;
	tnt_raise(UnsupportedIndexFeature, this, "random()");
	return NULL;
}

size_t
Index::count(enum iterator_type /* type */, const char* /* key */,
             uint32_t /* part_count */) const
{
	tnt_raise(UnsupportedIndexFeature, this, "count()");
	return 0;
}

struct tuple *
Index::findByKey(const char *key, uint32_t part_count) const
{
	(void) key;
	(void) part_count;
	tnt_raise(UnsupportedIndexFeature, this, "findByKey()");
	return NULL;
}

struct tuple *
Index::findByTuple(struct tuple *tuple) const
{
	(void) tuple;
	tnt_raise(UnsupportedIndexFeature, this, "findByTuple()");
	return NULL;
}

struct tuple *
Index::replace(struct tuple *old_tuple, struct tuple *new_tuple,
		     enum dup_replace_mode mode)
{
	(void) old_tuple;
	(void) new_tuple;
	(void) mode;
	tnt_raise(UnsupportedIndexFeature, this, "replace()");
	return NULL;
}

size_t
Index::bsize() const
{
	return 0;
}

void
Index::initIterator(struct iterator *ptr, enum iterator_type type,
		    const char *key, uint32_t part_count) const
{
	(void) ptr;
	(void) type;
	(void) key;
	(void) part_count;
	tnt_raise(UnsupportedIndexFeature, this, "requested iterator type");
}

/**
 * Create a read view for iterator so further index modifications
 * will not affect the iterator iteration.
 */
void
Index::createReadViewForIterator(struct iterator *iterator)
{
	(void) iterator;
	tnt_raise(UnsupportedIndexFeature, this, "consistent read view");
}

/**
 * Destroy a read view of an iterator. Must be called for iterators,
 * for which createReadViewForIterator was called.
 */
void
Index::destroyReadViewForIterator(struct iterator *iterator)
{
	(void) iterator;
	tnt_raise(UnsupportedIndexFeature, this, "consistent read view");
}

static inline Index *
check_index(uint32_t space_id, uint32_t index_id, struct space **space)
{
	*space = space_cache_find(space_id);
	access_check_space(*space, PRIV_R);
	return index_find_xc(*space, index_id);
}

static inline box_tuple_t *
tuple_bless_null_xc(struct tuple *tuple)
{
	if (tuple != NULL)
		return tuple_bless_xc(tuple);
	return NULL;
}

const box_key_def_t *
box_index_key_def(uint32_t space_id, uint32_t index_id)
{
	try {
		struct space *space;
		/* no tx management, len is approximate in vinyl anyway. */
		Index *index = check_index(space_id, index_id, &space);
		return &index->index_def->key_def;
	} catch (Exception *) {
		return NULL;
	}
}

ssize_t
box_index_len(uint32_t space_id, uint32_t index_id)
{
	try {
		struct space *space;
		/* no tx management, len is approximate in vinyl anyway. */
		return check_index(space_id, index_id, &space)->size();
	} catch (Exception *) {
		return (size_t) -1; /* handled by box.error() in Lua */
	}
}

ssize_t
box_index_bsize(uint32_t space_id, uint32_t index_id)
{
       try {
	       /* no tx management for statistics */
	       struct space *space;
	       return check_index(space_id, index_id, &space)->bsize();
       } catch (Exception *) {
               return (size_t) -1; /* handled by box.error() in Lua */
       }
}

int
box_index_random(uint32_t space_id, uint32_t index_id, uint32_t rnd,
		box_tuple_t **result)
{
	assert(result != NULL);
	try {
		struct space *space;
		/* no tx management, random() is for approximation anyway */
		Index *index = check_index(space_id, index_id, &space);
		struct tuple *tuple = index->random(rnd);
		*result = tuple_bless_null_xc(tuple);
		return 0;
	}  catch (Exception *) {
		return -1;
	}
}

int
box_index_get(uint32_t space_id, uint32_t index_id, const char *key,
	      const char *key_end, box_tuple_t **result)
{
	assert(key != NULL && key_end != NULL && result != NULL);
	mp_tuple_assert(key, key_end);
	try {
		struct space *space;
		Index *index = check_index(space_id, index_id, &space);
		if (!index->index_def->opts.is_unique)
			tnt_raise(ClientError, ER_MORE_THAN_ONE_TUPLE);
		uint32_t part_count = mp_decode_array(&key);
		if (primary_key_validate(&index->index_def->key_def, key,
					 part_count))
			diag_raise();
		/* Start transaction in the engine. */
		struct txn *txn = txn_begin_ro_stmt(space);
		struct tuple *tuple = index->findByKey(key, part_count);
		/* Count statistics */
		rmean_collect(rmean_box, IPROTO_SELECT, 1);

		*result = tuple_bless_null_xc(tuple);
		txn_commit_ro_stmt(txn);
		return 0;
	}  catch (Exception *) {
		txn_rollback_stmt();
		return -1;
	}
}

int
box_index_min(uint32_t space_id, uint32_t index_id, const char *key,
	      const char *key_end, box_tuple_t **result)
{
	assert(key != NULL && key_end != NULL && result != NULL);
	mp_tuple_assert(key, key_end);
	try {
		struct space *space;
		Index *index = check_index(space_id, index_id, &space);
		if (index->index_def->type != TREE) {
			/* Show nice error messages in Lua */
			tnt_raise(UnsupportedIndexFeature, index, "min()");
		}
		uint32_t part_count = mp_decode_array(&key);
		if (key_validate(index->index_def, ITER_GE, key, part_count))
			diag_raise();
		/* Start transaction in the engine */
		struct txn *txn = txn_begin_ro_stmt(space);
		struct tuple *tuple = index->min(key, part_count);
		*result = tuple_bless_null_xc(tuple);
		txn_commit_ro_stmt(txn);
		return 0;
	}  catch (Exception *) {
		txn_rollback_stmt();
		return -1;
	}
}

int
box_index_max(uint32_t space_id, uint32_t index_id, const char *key,
	      const char *key_end, box_tuple_t **result)
{
	mp_tuple_assert(key, key_end);
	assert(result != NULL);
	try {
		struct space *space;
		Index *index = check_index(space_id, index_id, &space);
		if (index->index_def->type != TREE) {
			/* Show nice error messages in Lua */
			tnt_raise(UnsupportedIndexFeature, index, "max()");
		}
		uint32_t part_count = mp_decode_array(&key);
		if (key_validate(index->index_def, ITER_LE, key, part_count))
			diag_raise();
		/* Start transaction in the engine */
		struct txn *txn = txn_begin_ro_stmt(space);
		struct tuple *tuple = index->max(key, part_count);
		*result = tuple_bless_null_xc(tuple);
		txn_commit_ro_stmt(txn);
		return 0;
	}  catch (Exception *) {
		txn_rollback_stmt();
		return -1;
	}
}

ssize_t
box_index_count(uint32_t space_id, uint32_t index_id, int type,
		const char *key, const char *key_end)
{
	assert(key != NULL && key_end != NULL);
	mp_tuple_assert(key, key_end);
	enum iterator_type itype = (enum iterator_type) type;
	try {
		struct space *space;
		Index *index = check_index(space_id, index_id, &space);
		uint32_t part_count = mp_decode_array(&key);
		if (key_validate(index->index_def, itype, key, part_count))
			diag_raise();
		/* Start transaction in the engine */
		struct txn *txn = txn_begin_ro_stmt(space);
		ssize_t count = index->count(itype, key, part_count);
		txn_commit_ro_stmt(txn);
		return count;
	} catch (Exception *) {
		txn_rollback_stmt();
		return -1; /* handled by box.error() in Lua */
	}
}

/* }}} */

/* {{{ Iterators ************************************************/

box_iterator_t *
box_index_iterator(uint32_t space_id, uint32_t index_id, int type,
                   const char *key, const char *key_end)
{
	assert(key != NULL && key_end != NULL);
	mp_tuple_assert(key, key_end);
	struct iterator *it = NULL;
	enum iterator_type itype = (enum iterator_type) type;
	try {
		struct space *space;
		Index *index = check_index(space_id, index_id, &space);
		struct txn *txn = txn_begin_ro_stmt(space);
		assert(mp_typeof(*key) == MP_ARRAY); /* checked by Lua */
		uint32_t part_count = mp_decode_array(&key);
		if (key_validate(index->index_def, itype, key, part_count))
			diag_raise();
		it = index->allocIterator();
		index->initIterator(it, itype, key, part_count);
		it->schema_version = schema_version;
		it->space_id = space_id;
		it->index_id = index_id;
		it->index = index;
		txn_commit_ro_stmt(txn);
		return it;
	} catch (Exception *) {
		if (it)
			it->free(it);
		/* will be hanled by box.error() in Lua */
		return NULL;
	}
}

int
box_iterator_next(box_iterator_t *itr, box_tuple_t **result)
{
	assert(result != NULL);
	assert(itr->next != NULL);
	try {
		if (itr->schema_version != schema_version) {
			struct space *space;
			/* no tx management */
			Index *index = check_index(itr->space_id, itr->index_id,
						   &space);
			if (index != itr->index) {
				*result = NULL;
				return 0;
			}
			if (index->schema_version > itr->schema_version) {
				*result = NULL; /* invalidate iterator */
				return 0;
			}
			itr->schema_version = schema_version;
		}
	} catch (Exception *) {
		*result = NULL;
		return 0; /* invalidate iterator */
	}
	try {
		struct tuple *tuple = itr->next(itr);
		*result = tuple_bless_null_xc(tuple);
		return 0;
	} catch (Exception *) {
		return -1;
	}
}

void
box_iterator_free(box_iterator_t *it)
{
	if (it->free)
		it->free(it);
}

/* }}} */

/* {{{ Introspection */

void
Index::info(struct info_handler *info) const
{
	info_begin(info);
	info_end(info);
}

int
box_index_info(uint32_t space_id, uint32_t index_id,
	       struct info_handler *info)
{
	try {
		struct space *space;
		Index *index = check_index(space_id, index_id, &space);
		index->info(info);
		return 0;
	}  catch (Exception *) {
		return -1;
	}
}

/* }}} */
