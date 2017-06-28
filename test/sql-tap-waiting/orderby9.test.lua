#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(4)

--!./tcltestrunner.lua
-- 2015-08-26
--
-- The author disclaims copyright to this source code.  In place of
-- a legal notice, here is a blessing:
--
--    May you do good and not evil.
--    May you find forgiveness for yourself and forgive others.
--    May you share freely, never taking more than you give.
--
-------------------------------------------------------------------------
-- This file implements regression tests for SQLite library.
-- 
-- This file seeks to verify that expressions (and especially functions)
-- that are in both the ORDER BY clause and the result set are only
-- evaluated once.
--
-- ["set","testdir",[["file","dirname",["argv0"]]]]
-- ["source",[["testdir"],"\/tester.tcl"]]

local function shallowcopy(orig)
    local orig_type = type(orig)
    local copy
    if orig_type == 'table' then
        copy = {}
        for orig_key, orig_value in pairs(orig) do
            copy[orig_key] = orig_value
        end
    else -- number, string, boolean, etc
        copy = orig
    end
    return copy
end

local testprefix = "orderby9"
test:do_execsql_test(
    "setup",
    [[
        -- create a table with many entries
        CREATE TABLE t1(x primary key);
        WITH RECURSIVE
           c(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<100)
        INSERT INTO t1 SELECT x FROM c;
    ]])

-- Some versions of TCL are unable to [lsort -int] for
-- 64-bit integers.  So we write our own comparison
-- routine.
local function bigintcompare(a, b)
    local x = (a - b)
    if (x < 0)
        then return -1
    end
    if (x > 0)
        then return 1
    end
    return 0
end

local l1 = test:execsql("SELECT random() AS y FROM t1 ORDER BY 1;")


test:do_test(
    1.0,
    function()
        -- If random() is only evaluated once and then reused for each row, then
        -- the output should appear in sorted order.  If random() is evaluated 
        -- separately for the result set and the ORDER BY clause, then the output
        -- order will be random.
        local l2 = shallowcopy(l1)
        return table.sort(l2) or l2
    end, l1)

l1 = test:execsql("SELECT random() AS y FROM t1 ORDER BY random();")

test:do_test(
    1.1,
    function()
        local l2 = shallowcopy(l1)
        return table.sort(l2) or l2
    end, l1)

l1 = test:execsql("SELECT random() AS y FROM t1 ORDER BY +random();")
test:do_test(
    1.2,
    function()
        local l2 = shallowcopy(l1)
        return table.sort(l2) or l2
    end, l1)



test:finish_test()
