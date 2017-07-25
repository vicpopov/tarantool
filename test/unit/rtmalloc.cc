#include "memory.h"
#include "fiber.h"
#include "rtmalloc.h"
#include "unit.h"
#include "trivia/util.h"

extern struct quota runtime_quota;
extern size_t runtime_quota_total;

void
check_oom_error()
{
	struct error *error = diag_last_error(&fiber()->diag);
	isnt(NULL, error, "diag is set");
	is(error->type, &type_OutOfMemory, "error is OutOfMemory");
	diag_clear(&fiber()->diag);
	fail_if(diag_last_error(&fiber()->diag) != NULL);
}

void
test_basic()
{
	struct quota_lessor *lessor = &cord()->runtime_quota;
	plan(14);
	is(quota_used(&runtime_quota), 0, "quota is empty");
	void *p = rtmalloc(100);
	isnt(p, NULL, "rtmalloc allocated");
	size_t used = quota_used(&runtime_quota);
	ok(used > 0, "quota accounted malloc");
	size_t leased = quota_leased(lessor);
	ok(leased >= 100, "leased >= 100");
	is(QUOTA_LEASE_SIZE - leased, quota_available(lessor),
	   "available 1Mb - malloced size");
	rtfree(p);
	is(used, quota_used(&runtime_quota), "lease end did not freed quota");
	is(QUOTA_LEASE_SIZE, quota_available(lessor),
	   "free returned memory to lessor");
	is(0, quota_leased(lessor), "leased zero");

	int *a = (int *) rtcalloc(5, sizeof(int));
	int s = 0;
	for (int i = 0; i < 5; ++i)
		s += a[i];
	is(s, 0, "rtcalloc sets zeros");
	leased = quota_leased(lessor);
	ok(leased > 0, "leased int[5] array");
	is(used, quota_used(&runtime_quota), "source quota did not change");

	a = (int *) rtrealloc(a, 10 * sizeof(int));
	ok(leased < quota_leased(lessor), "leased increased after realloc");
	rtfree(a);
	is(0, quota_leased(lessor), "leased zero");

	quota_end_total(lessor);
	is(0, quota_used(&runtime_quota), "source quota is freed");
	check_plan();
}

void
test_realloc_fail()
{
	plan(11);
	struct quota_lessor *lessor = &cord()->runtime_quota;
	is(0, quota_used(&runtime_quota), "quota is empty");
	void *p = rtrealloc(NULL, runtime_quota_total - QUOTA_UNIT_SIZE * 2);
	isnt(NULL, p, "realloc from NULL");
	size_t leased = quota_leased(lessor);
	ok(leased >= runtime_quota_total - QUOTA_UNIT_SIZE * 2, "leased realloc");
	size_t used = quota_used(&runtime_quota);
	ok(used >= leased, "quota used");
	void *p2 = rtrealloc(p, runtime_quota_total * 2);
	is(NULL, p2, "realloc failed");
	check_oom_error();
	is(leased, quota_leased(lessor), "leased did not changed");
	is(used, quota_used(&runtime_quota), "quota used did not changed");
	rtfree(p);
	is(0, quota_leased(lessor), "leased zero");
	quota_end_total(lessor);
	is(0, quota_used(&runtime_quota), "source quota is freed");
	check_plan();
}

void
test_basic_fails()
{
	plan(6);
	struct quota_lessor *lessor = &cord()->runtime_quota;
	void *p = rtmalloc(runtime_quota_total * 2);
	check_oom_error();

	p = rtcalloc(2, runtime_quota_total);
	check_oom_error();

	p = rtrealloc(NULL, runtime_quota_total * 2);
	check_oom_error();
	check_plan();
}

int
main()
{
	runtime_quota_total = QUOTA_LEASE_SIZE * 10;
	plan(3);
	memory_init();
	fiber_init(fiber_cxx_invoke);
	exception_init();

	test_basic();
	test_realloc_fail();
	test_basic_fails();

	fiber_free();
	memory_free();
	return check_plan();
}
