#include "memory.h"
#include "fiber.h"
#include "rtmalloc.h"
#include "unit.h"
#include "trivia/util.h"
#include "time.h"
#include "memtx_tuple.h"

#define Kb 1024LLU
#define Mb Kb * 1024LLU
#define Gb Mb * 1024LLU

extern struct small_alloc memtx_alloc;
extern uint32_t snapshot_version;
extern struct slab_arena memtx_arena;

const uint tuple_count = 1000000;
struct tuple *tuples[tuple_count];
const uint tuple_size_min = 32;
const uint tuple_size_max = Kb;
const uint iterations = 5;
char tuple[tuple_size_max];
uint64_t memtx_arena_size = 8LLU * Gb;

static inline unsigned long long
timeval_to_usec(const struct timeval *tv)
{
	return tv->tv_sec * 1000000LLU + tv->tv_usec;
}

static void
shuffle()
{
	for (uint i = 0; i < tuple_count / 2; ++i) {
		uint i1 = rand() % tuple_count;
		uint i2 = rand() % tuple_count;
		struct tuple *tmp = tuples[i1];
		tuples[i1] = tuples[i2];
		tuples[i2] = tmp;
	}
}

static void
destroy(uint count)
{
	for (uint i = 0; i < count; ++i) {
		if (tuples[i] != NULL)
			tuple_unref(tuples[i]);
		tuples[i] = NULL;
	}
}

static void
create()
{
	for (uint i = 0; i < tuple_count; ++i) {
		if (tuples[i] != NULL)
			return;
		uint size = ((uint)rand() + tuple_size_min) %
			    tuple_size_max + 1;
		tuples[i] = memtx_tuple_new(tuple_format_default, tuple,
					    tuple + size);
		assert(tuples[i] != NULL);
		tuple_ref(tuples[i]);
	}
}

int
main()
{
	srand(time(NULL));
	memory_init();
	fiber_init(fiber_cxx_invoke);
	exception_init();
	tuple_init();
	memtx_tuple_init(memtx_arena_size, 10, 4LLU * Mb, 1.1);
	snapshot_version = 0;
	struct quota *memtx_quota = memtx_arena.quota;
	struct quota_lessor *runtime_quota = &cord()->runtime_quota;

	memset(tuples, 0, sizeof(tuples));
	char *pos = mp_encode_array(tuple, 1);
	pos = mp_encode_uint(pos, 1);

	printf("initialization...\n");

	struct timeval start_time;
	gettimeofday(&start_time, NULL);

	for (uint iters = 0; iters < iterations; ++iters) {
		create();
		shuffle();
		destroy(tuple_count / 2);
	}

	struct timeval end_time;
	gettimeofday(&end_time, NULL);

	unsigned long long microsecs =
		timeval_to_usec(&end_time) - timeval_to_usec(&start_time);
	destroy(tuple_count);
	printf("initialization is finished in %llu microsecs\n", microsecs);
	printf("starting bench...\n");

	gettimeofday(&start_time, NULL);

	for (uint iters = 0; iters < iterations; ++iters) {
		create();
		shuffle();
		size_t mused = quota_used(memtx_quota);
		size_t rused = quota_leased(runtime_quota);
		printf("memtx used mem: %zu\nruntime used mem: %zu\n", mused,
		       rused);
		destroy(tuple_count / 2);
	}

	gettimeofday(&end_time, NULL);
	microsecs = timeval_to_usec(&end_time) - timeval_to_usec(&start_time);
	destroy(tuple_count);

	printf("bench finished in %llu microsecs\n", microsecs);

	memtx_tuple_free();
	tuple_free();
	fiber_free();
	memory_free();
	return 0;
}
