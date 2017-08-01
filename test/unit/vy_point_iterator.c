#include "trivia/util.h"
#include "unit.h"
#include "vy_index.h"
#include "vy_cache.h"
#include "fiber.h"
#include <small/lsregion.h>
#include <small/slab_cache.h>

uint64_t schema_version;

static void
test_basic()
{
	header();
	plan(4);

	const size_t QUOTA = 100 * 1024 * 1024;
	int64_t generation = 0;
	struct slab_cache *slab_cache = cord_slab_cache();
	struct lsregion lsregion;
	lsregion_create(&lsregion, slab_cache->arena);

	int rc;
	struct vy_index_env index_env;
	rc = vy_index_env_create(&index_env, ".", &lsregion, &generation,
				 NULL, NULL);
	is(rc, 0, "vy_index_env_create");

	struct vy_cache_env cache_env;
	vy_cache_env_create(&cache_env, slab_cache, QUOTA);

	struct vy_cache cache;
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def = box_key_def_new(fields, types, 1);
	isnt(key_def, NULL, "key_def is not NULL");

	vy_cache_create(&cache, &cache_env, key_def);

	struct tuple_format *format =
		tuple_format_new(&vy_tuple_format_vtab, &key_def, 1, 0);
	isnt(format, NULL, "tuple_format_new is not NULL");
	tuple_format_ref(format, 1);

	struct index_opts index_opts = index_opts_default;
	struct index_def *index_def =
		index_def_new(512, 0, "primary", sizeof("primary"), TREE,
			      &index_opts, key_def, NULL);

	struct vy_index *pk = vy_index_new(&index_env, &cache_env, index_def,
					   format, NULL, 1);
	isnt(pk, NULL, "index is not NULL")

	vy_index_unref(pk);
	index_def_delete(index_def);
	tuple_format_ref(format, -1);
	vy_cache_destroy(&cache);
	box_key_def_delete(key_def);
	vy_cache_env_destroy(&cache_env);
	vy_index_env_destroy(&index_env);

	lsregion_destroy(&lsregion);

	check_plan();
	footer();
}

int
main()
{
	plan(1);

	memory_init();
	fiber_init(fiber_c_invoke);
	tuple_init();

	test_basic();

	tuple_free();
	fiber_free();
	memory_free();

	return check_plan();
}
