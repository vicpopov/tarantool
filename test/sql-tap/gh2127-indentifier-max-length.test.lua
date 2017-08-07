#!/usr/bin/env tarantool
test = require("sqltester")

test:plan(5)

-- 30kb table name
local table_name = string.rep("АААААААААА", 300);

-- Execute CREATE TABLE statement with 30kb table identifier
test:do_execsql_test(
	"identifier-1.1",
	"CREATE TABLE " .. table_name .. "(a INT PRIMARY KEY);"
	, {
	    -- <identifier-1.1>
	    -- <identifier-1.1>
	})

-- Create 30kb view name
local view_name = string.rep("BBBBBBBBBB", 300);

test:do_execsql_test(
	"identifier-1.2",
	"CREATE VIEW " .. view_name .. " AS SELECT 1; "
	, {
	    -- <identifier-1.2>
	    -- <identifier-1.2>
	})

-- Create 30kb index name
local index_name = string.rep("ДДДДДДДДДД", 300);

test:execsql "CREATE TABLE t1(a INT PRIMARY KEY);"
test:do_execsql_test(
	"identifier-1.3",
	"CREATE INDEX " .. index_name .. " ON t1(a);"
	, {
	   -- <identifier-1.3>
	   --
	})

-- Create 30kb trigger name
local trigger_name = string.rep("ССССССССС", 300)

test:do_execsql_test(
	"identifier-1.4",
	"CREATE TRIGGER " .. trigger_name ..
	[[
	BEFORE UPDATE ON t1
	BEGIN
		SELECT 1;
	END;
	]]
	, {
	    -- <identifier-1.4>
	    -- <identifier-1.4>
	})

-- Create 90kb table name
local big_table_name = string.rep("ЕЕЕЕЕЕЕЕЕЕ", 90000)

-- Should error here
test:do_catchsql_test(
	"identifier-1.5",
	"CREATE TABLE " .. big_table_name .. "(a INT PRIMARY KEY);"
	, {
	    -- <identifier-1.5>
	1, "Failed to create space \'ЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕЕ\': space name is too long"
	    -- <identifier-1.5>
	})

test:finish_test()
