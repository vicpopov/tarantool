#include "module.h"

#include <stdio.h>
#include <msgpuck.h>

int
foo(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	static const char *SPACE_TEST_NAME = "test";
	uint32_t space_test_id = box_space_id_by_name(SPACE_TEST_NAME,
			strlen(SPACE_TEST_NAME));
	if (space_test_id == BOX_ID_NIL) {
		return box_error_set(__FILE__, __LINE__, ER_PROC_C,
			"Can't find space %s", SPACE_TEST_NAME);
	}
	char buf[16];
	char *end = buf;
	end = mp_encode_array(end, 1);
	end = mp_encode_uint(end, 0);
	if (box_insert(space_test_id, buf, end, NULL) < 0) {
		return box_error_set(__FILE__, __LINE__, ER_PROC_C,
			"Can't insert in space %s", SPACE_TEST_NAME);
	}
	return 0;
}
