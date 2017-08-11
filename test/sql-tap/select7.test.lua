#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(18)

--!./tcltestrunner.lua
-- The author disclaims copyright to this source code.  In place of
-- a legal notice, here is a blessing:
--
--    May you do good and not evil.
--    May you find forgiveness for yourself and forgive others.
--    May you share freely, never taking more than you give.
--
-------------------------------------------------------------------------
-- This file implements regression tests for SQLite library.  The
-- focus of this file is testing compute SELECT statements and nested
-- views.
--
-- $Id: select7.test,v 1.11 2007/09/12 17:01:45 danielk1977 Exp $
-- ["set","testdir",[["file","dirname",["argv0"]]]]
-- ["source",[["testdir"],"\/tester.tcl"]]
testprefix = "select7"
-- A 3-way INTERSECT.  Ticket #875
test:do_execsql_test(
    "select7-1.1",
    [[
        drop table if exists t1;
        create table t1(x primary key);
        insert into t1 values('amx');
        insert into t1 values('anx');
        insert into t1 values('amy');
        insert into t1 values('bmy');
        select * from t1 where x like 'a__'
          intersect select * from t1 where x like '_m_'
          intersect select * from t1 where x like '__x';
    ]], {
        -- <select7-1.1>
        "amx"
        -- </select7-1.1>
    })



-- MUST_WORK_TEST
-- # Nested views do not handle * properly.  Ticket #826.
-- #
-- ifcapable view {
-- do_test select7-2.1 {
--   execsql {
--     CREATE TABLE x(id integer primary key, a TEXT NULL);
--     INSERT INTO x (a) VALUES ('first');
--     CREATE TABLE tempx(id integer primary key, a TEXT NULL);
--     INSERT INTO tempx (a) VALUES ('t-first');
--     CREATE VIEW tv1 AS SELECT x.id, tx.id FROM x JOIN tempx tx ON tx.id=x.id;
--     CREATE VIEW tv1b AS SELECT x.id, tx.id FROM x JOIN tempx tx on tx.id=x.id;
--     CREATE VIEW tv2 AS SELECT * FROM tv1 UNION SELECT * FROM tv1b;
--     SELECT * FROM tv2;
--   }
-- } {1 1}
-- } ;# ifcapable view


-- ifcapable compound
-- # Do not allow GROUP BY without an aggregate. Ticket #1039.
-- #
-- # Change: force any query with a GROUP BY clause to be processed as
-- # an aggregate query, whether it contains aggregates or not.
-- #
-- ifcapable subquery {
--   # do_test select7-3.1 {
--   #   catchsql {
--   #     SELECT * FROM (SELECT * FROM sqlite_master) GROUP BY name
--   #   }
--   # } {1 {GROUP BY may only be used on aggregate queries}}
--   do_test select7-3.1 {
--     catchsql {
--       SELECT * FROM (SELECT * FROM sqlite_master) GROUP BY name
--     }
--   } [list 0 [execsql {SELECT * FROM sqlite_master ORDER BY name}]]
-- }
-- Ticket #2018 - Make sure names are resolved correctly on all
-- SELECT statements of a compound subquery.
--

test:do_execsql_test(
    "select7-4.1",
    [[
        DROP TABLE IF EXISTS photo;
        DROP TABLE IF EXISTS tag;
        CREATE TABLE IF NOT EXISTS photo(pk integer primary key, x);
        CREATE TABLE IF NOT EXISTS tag(pk integer primary key, fk int, name);

        SELECT P.pk from PHOTO P WHERE NOT EXISTS (
             SELECT T2.pk from TAG T2 WHERE T2.fk = P.pk
             EXCEPT
             SELECT T3.pk from TAG T3 WHERE T3.fk = P.pk AND T3.name LIKE '%foo%'
        );
    ]], {
        -- <select7-4.1>

        -- </select7-4.1>
    })

test:do_execsql_test(
    "select7-4.2",
    [[
        INSERT INTO photo VALUES(1,1);
        INSERT INTO photo VALUES(2,2);
        INSERT INTO photo VALUES(3,3);
        INSERT INTO tag VALUES(11,1,'one');
        INSERT INTO tag VALUES(12,1,'two');
        INSERT INTO tag VALUES(21,1,'one-b');
        SELECT P.pk from PHOTO P WHERE NOT EXISTS (
             SELECT T2.pk from TAG T2 WHERE T2.fk = P.pk
             EXCEPT
             SELECT T3.pk from TAG T3 WHERE T3.fk = P.pk AND T3.name LIKE '%foo%'
        );
    ]], {
        -- <select7-4.2>
        2, 3
        -- </select7-4.2>
    })


-- ticket #2347
--
test:do_catchsql_test(
    "select7-5.1",
    [[
        CREATE TABLE t2(a primary key,b);
        SELECT 5 IN (SELECT a,b FROM t2);
    ]], {
        -- <select7-5.1>
        1, "sub-select returns 2 columns - expected 1"
        -- </select7-5.1>
    })

test:do_catchsql_test(
    "select7-5.2",
    [[
        SELECT 5 IN (SELECT * FROM t2);
    ]], {
        -- <select7-5.2>
        1, "sub-select returns 2 columns - expected 1"
        -- </select7-5.2>
    })

test:do_catchsql_test(
    "select7-5.3",
    [[
        SELECT 5 IN (SELECT a,b FROM t2 UNION SELECT b,a FROM t2);
    ]], {
        -- <select7-5.3>
        1, "sub-select returns 2 columns - expected 1"
        -- </select7-5.3>
    })

test:do_catchsql_test(
    "select7-5.4",
    [[
        SELECT 5 IN (SELECT * FROM t2 UNION SELECT * FROM t2);
    ]], {
        -- <select7-5.4>
        1, "sub-select returns 2 columns - expected 1"
        -- </select7-5.4>
    })


-- Verify that an error occurs if you have too many terms on a
-- compound select statement.

-- hardcoded define from src
-- 500 is default value
local SQLITE_MAX_COMPOUND_SELECT = 500
sql = "SELECT 0"
for i = 0, SQLITE_MAX_COMPOUND_SELECT + 1, 1 do
    sql = sql .. " UNION ALL SELECT "..i..""
end
test:do_catchsql_test(
    "select7-6.2",
    sql, {
        -- <select7-6.2>
        1, "Too many UNION or EXCEPT or INTERSECT operations"
        -- </select7-6.2>
    })

-- This block of tests verifies that bug aa92c76cd4 is fixed.
--
test:do_execsql_test(
    "select7-7.1",
    [[
        CREATE TABLE t3(a REAL primary key);
        INSERT INTO t3 VALUES(44.0);
        INSERT INTO t3 VALUES(56.0);
    ]], {
        -- <select7-7.1>
        
        -- </select7-7.1>
    })

test:do_execsql_test(
    "select7-7.2",
    [[
        pragma vdbe_trace = 0;
        SELECT (CASE WHEN a=0 THEN 0 ELSE (a + 25) / 50 END) AS categ, count(*)
        FROM t3 GROUP BY categ
    ]], {
        -- <select7-7.2>
        1.38, 1, 1.62, 1
        -- </select7-7.2>
    })

test:do_execsql_test(
    "select7-7.3",
    [[
        CREATE TABLE t4(a REAL primary key);
        INSERT INTO t4 VALUES( 2.0 );
        INSERT INTO t4 VALUES( 3.0 );
    ]], {
        -- <select7-7.3>
        
        -- </select7-7.3>
    })

test:do_execsql_test(
    "select7-7.4",
    [[
        SELECT (CASE WHEN a=0 THEN 'zero' ELSE a/2 END) AS t FROM t4 GROUP BY t;
    ]], {
        -- <select7-7.4>
        1.0, 1.5
        -- </select7-7.4>
    })

test:do_execsql_test(
    "select7-7.5",
    [[
        SELECT a=0, typeof(a) FROM t4 
    ]], {
        -- <select7-7.5>
        0, "real", 0, "real"
        -- </select7-7.5>
    })

test:do_execsql_test(
    "select7-7.6",
    [[
        SELECT a=0, typeof(a) FROM t4 GROUP BY a 
    ]], {
        -- <select7-7.6>
        0, "real", 0, "real"
        -- </select7-7.6>
    })

test:do_execsql_test(
    "select7-7.7",
    [[
        DROP TABLE IF EXISTS t5;
        CREATE TABLE t5(a TEXT primary key, b INT);
        INSERT INTO t5 VALUES(123, 456);
        SELECT typeof(a), a FROM t5 GROUP BY a HAVING a<b;
    ]], {
        -- <select7-7.7>
        "text", "123"
        -- </select7-7.7>
    })

test:do_execsql_test(
    8.0,
    [[
        CREATE TABLE t01(x primary key, y);
        CREATE TABLE t02(x primary key, y);
    ]])

test:do_catchsql_test(
    8.1,
    [[
        SELECT * FROM (
          SELECT * FROM t01 UNION SELECT x FROM t02
        ) WHERE y=1
    ]], {
        -- <8.1>
        1, "SELECTs to the left and right of UNION do not have the same number of result columns"
        -- </8.1>
    })

test:do_catchsql_test(
    8.2,
    [[
        CREATE VIEW v0 as SELECT x, y FROM t01 UNION SELECT x FROM t02;
        EXPLAIN QUERY PLAN SELECT * FROM v0 WHERE x='0' OR y;
    ]], {
        -- <8.2>
        1, "SELECTs to the left and right of UNION do not have the same number of result columns"
        -- </8.2>
    })

test:finish_test()

