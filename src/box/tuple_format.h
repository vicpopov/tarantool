#ifndef TARANTOOL_BOX_TUPLE_FORMAT_H_INCLUDED
#define TARANTOOL_BOX_TUPLE_FORMAT_H_INCLUDED
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

#include "key_def.h" /* for enum field_type */
#include "errinj.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/**
 * Destroy tuple format subsystem and free resourses
 */
void
tuple_format_free();

enum { FORMAT_ID_MAX = UINT16_MAX - 1, FORMAT_ID_NIL = UINT16_MAX };
enum { FORMAT_REF_MAX = INT32_MAX};

/*
 * We don't pass TUPLE_INDEX_BASE around dynamically all the time,
 * at least hard code it so that in most cases it's a nice error
 * message
 */
enum { TUPLE_INDEX_BASE = 1 };

/*
 * A special value to indicate that tuple format doesn't store
 * an offset for a field_id.
 */
enum { TUPLE_OFFSET_SLOT_NIL = INT32_MAX };

/**
 * @brief Tuple field format
 * Support structure for struct tuple_format.
 * Contains information of one field.
 */
struct tuple_field_format {
	/**
	 * Field type of an indexed field.
	 * If a field participates in at least one of space indexes
	 * then its type is stored in this member.
	 * If a field does not participate in an index
	 * then UNKNOWN is stored for it.
	 */
	enum field_type type;
	/**
	 * Offset slot in field map in tuple.
	 * Normally tuple stores field map - offsets of all fields
	 * participating in indexes. This allows quick access to most
	 * used fields without parsing entire mspack.
	 * This member stores position in the field map of tuple
	 * for current field.
	 * If the field does not participate in indexes then it has
	 * no offset in field map and INT_MAX is stored in this member.
	 * Due to specific field map in tuple (it is stored before tuple),
	 * the positions in field map is negative.
	 * Thus if this member is negative, smth like
	 * tuple->data[((uint32_t *)tuple)[format->offset_slot[fieldno]]]
	 * gives the start of the field
	 */
	int32_t offset_slot;
};

struct tuple;
struct tuple_format;

/** Engine-specific tuple format methods. */
struct tuple_format_vtab {
	/** Free allocated tuple using engine-specific memory allocator. */
	void
	(*destroy)(struct tuple_format *format, struct tuple *tuple);
};

/**
 * @brief Tuple format
 * Tuple format describes how tuple is stored and information about its fields
 */
struct tuple_format {
	/** Virtual function table */
	struct tuple_format_vtab vtab;
	/** Identifier */
	uint16_t id;
	/** Reference counter */
	int refs;
	/**
	 * The number of extra bytes to reserve in tuples before
	 * field map.
	 * \sa struct tuple
	 */
	uint16_t extra_size;
	/**
	 * Size of field map of tuple in bytes.
	 * \sa struct tuple
	 */
	uint16_t field_map_size;
	/**
	 * If not set (== 0), any tuple in the space can have any number of
	 * fields. If set, each tuple must have exactly this number of fields.
	 */
	uint32_t exact_field_count;
	/* Length of 'fields' array. */
	uint32_t field_count;
	/* Formats of the fields */
	struct tuple_field_format fields[];
};

extern struct tuple_format **tuple_formats;

static inline uint32_t
tuple_format_id(const struct tuple_format *format)
{
	assert(tuple_formats[format->id] == format);
	return format->id;
}

static inline struct tuple_format *
tuple_format_by_id(uint32_t tuple_format_id)
{
	return tuple_formats[tuple_format_id];
}

/** Delete a format with zero ref count. */
void
tuple_format_delete(struct tuple_format *format);

static inline void
tuple_format_ref(struct tuple_format *format)
{
	assert((uint64_t)format->refs + 1 <= FORMAT_REF_MAX);
	format->refs++;
}

static inline void
tuple_format_unref(struct tuple_format *format)
{
	assert(format->refs >= 1);
	if (--format->refs == 0)
		tuple_format_delete(format);
}

/**
 * Allocate, construct and register a new in-memory tuple format.
 * @param vtab             Virtual function table for specific engines.
 * @param keys             Array of key_defs of a space.
 * @param key_count        The number of keys in @a keys array.
 * @param extra_size       Extra bytes to reserve in tuples metadata.
 *
 * @retval not NULL Tuple format.
 * @retval     NULL Memory error.
 */
struct tuple_format *
tuple_format_new(struct tuple_format_vtab *vtab, struct key_def **keys,
		 uint16_t key_count, uint16_t extra_size);

/**
 * Register the duplicate of the specified format.
 * @param src Original format.
 *
 * @retval not NULL Success.
 * @retval     NULL Memory or format register error.
 */
struct tuple_format *
tuple_format_dup(const struct tuple_format *src);

/**
 * Returns the total size of tuple metadata of this format.
 * See @link struct tuple @endlink for explanation of tuple layout.
 *
 * @param format Tuple Format.
 * @returns the total size of tuple metadata
 */
static inline uint16_t
tuple_format_meta_size(const struct tuple_format *format)
{
	return format->extra_size + format->field_map_size;
}

typedef struct tuple_format box_tuple_format_t;

/** \cond public */

/**
 * Return new in-memory tuple format based on passed key definitions.
 *
 * \param keys array of keys defined for the format
 * \key_count count of keys
 * \retval new tuple format if success
 * \retval NULL for error
 */
box_tuple_format_t *
box_tuple_format_new(struct key_def **keys, uint16_t key_count);

/**
 * Increment tuple format ref count.
 *
 * \param tuple_format the tuple format to ref
 */
void
box_tuple_format_ref(box_tuple_format_t *format);

/**
 * Decrement tuple format ref count.
 *
 * \param tuple_format the tuple format to unref
 */
void
box_tuple_format_unref(box_tuple_format_t *format);

/** \endcond public */

/**
 * Fill the field map of tuple with field offsets.
 * @param format    Tuple format.
 * @param field_map A pointer behind the last element of the field
 *                  map.
 * @param tuple     MessagePack array.
 *
 * @retval  0 Success.
 * @retval -1 Format error.
 *            +-------------------+
 * Result:    | offN | ... | off1 |
 *            +-------------------+
 *                                ^
 *                             field_map
 * tuple + off_i = indexed_field_i;
 */
int
tuple_init_field_map(const struct tuple_format *format, uint32_t *field_map,
		     const char *tuple);

/**
 * Get a field at the specific position in this MessagePack array.
 * Returns a pointer to MessagePack data.
 * @param format tuple format
 * @param tuple a pointer to MessagePack array
 * @param field_map a pointer to the LAST element of field map
 * @param field_no the index of field to return
 *
 * @returns field data if field exists or NULL
 * @sa tuple_init_field_map()
 */
static inline const char *
tuple_field_raw(const struct tuple_format *format, const char *tuple,
		const uint32_t *field_map, uint32_t field_no)
{
	if (likely(field_no < format->field_count)) {
		/* Indexed field */

		if (field_no == 0) {
			mp_decode_array(&tuple);
			return tuple;
		}

		int32_t offset_slot = format->fields[field_no].offset_slot;
		if (offset_slot != TUPLE_OFFSET_SLOT_NIL)
			return tuple + field_map[offset_slot];
	}
	ERROR_INJECT(ERRINJ_TUPLE_FIELD, return NULL);
	uint32_t field_count = mp_decode_array(&tuple);
	if (unlikely(field_no >= field_count))
		return NULL;
	for (uint32_t k = 0; k < field_no; k++)
		mp_next(&tuple);
	return tuple;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* #ifndef TARANTOOL_BOX_TUPLE_FORMAT_H_INCLUDED */
