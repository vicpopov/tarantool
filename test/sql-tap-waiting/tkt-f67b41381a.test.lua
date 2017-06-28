#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(1)

--!./tcltestrunner.lua
-- 2014 April 26
--
-- The author disclaims copyright to this source code.  In place of
-- a legal notice, here is a blessing:
--
--    May you do good and not evil.
--    May you find forgiveness for yourself and forgive others.
--    May you share freely, never taking more than you give.
--
-------------------------------------------------------------------------
-- Test that ticket f67b41381a has been resolved.
--
-- ["set","testdir",[["file","dirname",["argv0"]]]]
-- ["source",[["testdir"],"\/tester.tcl"]]
testprefix = "tkt-f67b41381a"
-- MUST_WORK
local json = require("json")
if (1 > 0)
 then

    test:execsql([[
        CREATE TABLE t1(a PRIMARY KEY, b);
        INSERT INTO t1 VALUES(1, 2);
        CREATE TABLE t2(a PRIMARY KEY, b);
        INSERT INTO t2 SELECT * FROM t1;
    ]])
    -- MUST_WORK alter table
--    test:do_execsql_test(
--        1.0,
--        [[
--            CREATE TABLE t1(a PRIMARY KEY);
--            INSERT INTO t1 VALUES(1);
--            ALTER TABLE t1 ADD COLUMN b DEFAULT 2;
--            CREATE TABLE t2(a PRIMARY KEY, b);
--            INSERT INTO t2 SELECT * FROM t1;
--            SELECT * FROM t2;
--        ]], {
--            -- <1.0>
--            1, 2
--            -- </1.0>
--        })

    --db("cache", "size", 0)
    local data ={
        {"CREATE TABLE t1(a PRIMARY KEY, b); CREATE TABLE t2(a PRIMARY KEY, b)", 1},
        {"CREATE TABLE t1(a PRIMARY KEY, b DEFAULT 'x'); CREATE TABLE t2(a PRIMARY KEY, b)", 0},
        {"CREATE TABLE t1(a PRIMARY KEY, b DEFAULT 'x'); CREATE TABLE t2(a PRIMARY KEY, b DEFAULT 'x')",1},
        {"CREATE TABLE t1(a PRIMARY KEY, b DEFAULT NULL); CREATE TABLE t2(a PRIMARY KEY, b)", 0},
        {"CREATE TABLE t1(a PRIMARY KEY DEFAULT 2, b); CREATE TABLE t2(a PRIMARY KEY DEFAULT 1, b)", 1},
        {"CREATE TABLE t1(a PRIMARY KEY DEFAULT 1, b); CREATE TABLE t2(a PRIMARY KEY DEFAULT 1, b)", 1},
        {"CREATE TABLE t1(a PRIMARY KEY DEFAULT 1, b DEFAULT 1); CREATE TABLE t2(a PRIMARY KEY DEFAULT 3, b DEFAULT 1)", 1},
        {"CREATE TABLE t1(a PRIMARY KEY DEFAULT 1, b DEFAULT 1); CREATE TABLE t2(a PRIMARY KEY DEFAULT 3, b DEFAULT 3)", 0},
    }
    for tn, val in ipairs(data) do
        local tbls = val[1]
        local xfer = val[2]
        test:execsql(" DROP TABLE t1; DROP TABLE t2 ")
        test:execsql(tbls)

        local res = 1
        local explain_data = box.sql.execute("EXPLAIN INSERT INTO t1 SELECT * FROM t2 ")
        for i, explain_line in ipairs(explain_data) do
            local opcode = explain_line[2]
            if opcode == "Column" then
                res = 0
            end
        end
        test:do_test(
            "2."..tn,
            function ()
                return res
            end,
            xfer)
    end
end
test:finish_test()

