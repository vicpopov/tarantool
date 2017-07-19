#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(2)

-- TEMP tables are temporarely omitted
test:do_catchsql_test(
	"trigger2-10.1",
	[[
		CREATE TEMP TABLE tmp1 (id INTEGER PRIMARY KEY);
	]], {
		-- <trigger2-10.1>
	1, 'near "TABLE": syntax error'	
		-- <trigger2-10.1>
});

-- TEMP triggers are removed now, check it
test:do_catchsql_test(
	"trigger2-10.1",
	[[
		CREATE TABLE t1 (id INTEGER PRIMARY KEY);
		CREATE TEMP TRIGGER ttmp1 BEFORE UPDATE ON t1
		BEGIN
			SELECT 1;
		END;
	]], {
		-- <trigger2-10.1>
	1, 'near "TRIGGER": syntax error'	
		-- <trigger2-10.1>
});


test:finish_test()
