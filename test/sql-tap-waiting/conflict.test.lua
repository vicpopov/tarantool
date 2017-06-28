#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(72)

--!./tcltestrunner.lua
-- 2002 January 29
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
-- This file implements tests for the conflict resolution extension
-- to SQLite.
--
-- $Id: conflict.test,v 1.32 2009/04/30 09:10:38 danielk1977 Exp $
-- ["set","testdir",[["file","dirname",["argv0"]]]]
-- ["source",[["testdir"],"\/tester.tcl"]]



-- MUST_WORK
if (1 > 0)
 then
    -- Create tables for the first group of tests.
    --
    test:do_execsql_test(
        "conflict-1.0",
        [[
            CREATE TABLE t1(a, b, c, UNIQUE(a,b), PRIMARY KEY(a,b));
            CREATE TABLE t2(x primary key);
            SELECT c FROM t1 ORDER BY c;
        ]], {
            -- <conflict-1.0>
            
            -- </conflict-1.0>
        })

    -- Six columns of configuration data as follows:
    --
    --   i      The reference number of the test
    --   cmd    An INSERT or REPLACE command to execute against table t1
    --   t0     True if there is an error from $cmd
    --   t1     Content of "c" column of t1 assuming no error in $cmd
    --   t2     Content of "x" column of t2
    --   t3     Number of temporary files created by this test
    --
    local queries = {
        {"INSERT" ,1, "", {1},  0, },
        {"INSERT OR IGNORE",0, {3}, {1}, 0},
        {"INSERT OR REPLACE", 0, {4}, {1}, 0},
        {"REPLACE", 0, 4, {1},  0,},
        {"INSERT OR FAIL", 1, {}, {1},  0},
        {"INSERT OR ABORT", 1, {}, {1},  0},
        {"INSERT OR ROLLBACK", 1, {}, {}, 0},
    }
    for i, val in ipairs(queries) do
        local cmd = val[1]
        local t0 = val[2]
        local t1 = val[3]
        local t2 = val[4]
        local t3 = val[5]
        test:do_test(
            "conflict-1."..i,
            function()
                -- MUST WORK  pass sqlite_opentemp_count variable to lua
                --sqlite_opentemp_count = 0
                local r1
                local r0, error = pcall(
                    function ()
                        test:execsql(string.format([[
                        DELETE FROM t1;
                        DELETE FROM t2;
                        INSERT INTO t1 VALUES(1,2,3);
                        BEGIN;
                        INSERT INTO t2 VALUES(1);
                        %s INTO t1 VALUES(1,2,4);
                        ]], cmd))
                    end
                )
                pcall(function () test:execsql("COMMIT;") end)
                r0 = r0 == true and 0 or 1
                if r0 == 1 then
                    print(error)
                    r1 = ""
                else
                    r1 = test:execsql("SELECT c FROM t1")
                end
                local r2 = test:execsql("SELECT x FROM t2;")
                -- MUST WORK  pass sqlite_opentemp_count variable to lua
                --r3 = sqlite_opentemp_count
                --return { r0, r1, r2, r3 }
                return { r0, r1, r2, 0}
            end, {
                --t0, t1, t2
                t0, t1, t2, t3
            })

    end
    -- Create tables for the first group of tests.
    --
    test:do_execsql_test(
        "conflict-2.0",
        [[
            DROP TABLE t1;
            DROP TABLE t2;
            CREATE TABLE t1(a INTEGER PRIMARY KEY, b, c, UNIQUE(a,b));
            CREATE TABLE t2(x);
            SELECT c FROM t1 ORDER BY c;
        ]], {
            -- <conflict-2.0>
            
            -- </conflict-2.0>
        })

    -- Six columns of configuration data as follows:
    --
    --   i      The reference number of the test
    --   cmd    An INSERT or REPLACE command to execute against table t1
    --   t0     True if there is an error from $cmd
    --   t1     Content of "c" column of t1 assuming no error in $cmd
    --   t2     Content of "x" column of t2
    --
    local queries = {
        {"INSERT", 1, {},  1},
        {"INSERT OR IGNORE", 0,3, 1},
        {"INSERT OR REPLACE", 0, 4, 1},
        {"REPLACE", 0, 4, 1},
        {"INSERT OR FAIL", 1, {}, 1, 6},
        {"INSERT OR ABORT", 1, {},  1, 7},
        {"INSERT OR ROLLBACK", 1, {},  {}},
        {"INSERT" ,1, "", 1,  0, },
        {"INSERT OR IGNORE",0, 3, 1, 0},
        {"INSERT OR REPLACE", 0, 4, 1, 0},
        {"REPLACE", 0, 4, 1,  0,},
        {"INSERT OR FAIL", 1, {}, 1,  0},
        {"INSERT OR ABORT", 1, {}, 1,  0},
        {"INSERT OR ROLLBACK", 1, {}, {}, 0},
    }
    for i, val in ipairs(queries) do
        local cmd = val[1]
        local t0 = val[2]
        local t1 = val[3]
        local t2 = val[4]
        test:do_test(
            "conflict-2."..i,
            function()
                local r1
                local r0 = pcall(function()
                    r1 = test:execsql(string.format([[
                        DELETE FROM t1;
                        DELETE FROM t2;
                        INSERT INTO t1 VALUES(1,2,3);
                        BEGIN;
                        INSERT INTO t2 VALUES(1); 
                        %s INTO t1 VALUES(1,2,4);
                    ]], cmd))
                    end)
                r0 = r0 == true and {0} or {1} 
                pcall( test:execsql("COMMIT"))
                if r0 == 1 then
                    r1 = ""
                end
                local r2 = test:execsql("SELECT x FROM t2")
                return { r0, r1, r2 }
            end, {
                t0, t1, t2
            })

    end
    -- Create tables for the first group of tests.
    --
    test:do_execsql_test(
        "conflict-3.0",
        [[
            DROP TABLE t1;
            DROP TABLE t2;
            CREATE TABLE t1(a, b, c INTEGER, PRIMARY KEY(c), UNIQUE(a,b));
            CREATE TABLE t2(x);
            SELECT c FROM t1 ORDER BY c;
        ]], {
            -- <conflict-3.0>
            
            -- </conflict-3.0>
        })

    -- Six columns of configuration data as follows:
    --
    --   i      The reference number of the test
    --   cmd    An INSERT or REPLACE command to execute against table t1
    --   t0     True if there is an error from $cmd
    --   t1     Content of "c" column of t1 assuming no error in $cmd
    --   t2     Content of "x" column of t2
    --
    for _ in X(0, "X!foreach", [=[["i cmd t0 t1 t2","\n     1 INSERT                  1 {}  1\n     2 {INSERT OR IGNORE}      0 3   1\n     3 {INSERT OR REPLACE}     0 4   1\n     4 REPLACE                 0 4   1\n     5 {INSERT OR FAIL}        1 {}  1\n     6 {INSERT OR ABORT}       1 {}  1\n     7 {INSERT OR ROLLBACK}    1 {}  {}\n   "]]=]) do
        test:do_test(
            "conflict-3."..i,
            function()
                local r1
                local r0 = pcall(function()
                    r1 = test:execsql(string.format([[
                        DELETE FROM t1;
                        DELETE FROM t2;
                        INSERT INTO t1 VALUES(1,2,3);
                        BEGIN;
                        INSERT INTO t2 VALUES(1); 
                        %s INTO t1 VALUES(1,2,4);
                    ]], cmd))
                    end)
                r0 = r0 == true and {0} or {1} 
                X(154, "X!cmd", [=[["catch","execsql {COMMIT}"]]=])
                if r0
 then
                    r1 = ""
                end
                r2 = test:execsql "SELECT x FROM t2"
                return { r0, r1, r2 }
            end, {
                t0, t1, t2
            })

    end
    test:do_execsql_test(
        "conflict-4.0",
        [[
            DROP TABLE t2;
            CREATE TABLE t2(x);
            SELECT x FROM t2;
        ]], {
            -- <conflict-4.0>
            
            -- </conflict-4.0>
        })

    -- Six columns of configuration data as follows:
    --
    --   i      The reference number of the test
    --   conf1  The conflict resolution algorithm on the UNIQUE constraint
    --   cmd    An INSERT or REPLACE command to execute against table t1
    --   t0     True if there is an error from $cmd
    --   t1     Content of "c" column of t1 assuming no error in $cmd
    --   t2     Content of "x" column of t2
    --
    for _ in X(0, "X!foreach", [=[["i conf1 cmd t0 t1 t2","\n     1 {}       INSERT                  1 {}  1\n     2 REPLACE  INSERT                  0 4   1\n     3 IGNORE   INSERT                  0 3   1\n     4 FAIL     INSERT                  1 {}  1\n     5 ABORT    INSERT                  1 {}  1\n     6 ROLLBACK INSERT                  1 {}  {}\n     7 REPLACE  {INSERT OR IGNORE}      0 3   1\n     8 IGNORE   {INSERT OR REPLACE}     0 4   1\n     9 FAIL     {INSERT OR IGNORE}      0 3   1\n    10 ABORT    {INSERT OR REPLACE}     0 4   1\n    11 ROLLBACK {INSERT OR IGNORE }     0 3   1\n   "]]=]) do
        test:do_test(
            "conflict-4."..i,
            function()
                if (conf1 ~= "")
 then
                    conf1 = "ON CONFLICT "..conf1..""
                end
                r0 = X(190, "X!cmd", [=[["catch","execsql [subst {\n         DROP TABLE t1;\n         CREATE TABLE t1(a,b,c,UNIQUE(a,b) $conf1);\n         DELETE FROM t2;\n         INSERT INTO t1 VALUES(1,2,3);\n         BEGIN;\n         INSERT INTO t2 VALUES(1); \n         $cmd INTO t1 VALUES(1,2,4);\n       }]","r1"]]=])
                X(198, "X!cmd", [=[["catch","execsql {COMMIT}"]]=])
                if r0
 then
                    r1 = ""
                end
                r2 = test:execsql "SELECT x FROM t2"
                return { r0, r1, r2 }
            end, {
                t0, t1, t2
            })

    end
    test:do_execsql_test(
        "conflict-5.0",
        [[
            DROP TABLE t2;
            CREATE TABLE t2(x);
            SELECT x FROM t2;
        ]], {
            -- <conflict-5.0>
            
            -- </conflict-5.0>
        })

    -- Six columns of configuration data as follows:
    --
    --   i      The reference number of the test
    --   conf1  The conflict resolution algorithm on the NOT NULL constraint
    --   cmd    An INSERT or REPLACE command to execute against table t1
    --   t0     True if there is an error from $cmd
    --   t1     Content of "c" column of t1 assuming no error in $cmd
    --   t2     Content of "x" column of t2
    --
    for _ in X(0, "X!foreach", [=[["i conf1 cmd t0 t1 t2","\n     1 {}       INSERT                  1 {}  1\n     2 REPLACE  INSERT                  0 5   1\n     3 IGNORE   INSERT                  0 {}  1\n     4 FAIL     INSERT                  1 {}  1\n     5 ABORT    INSERT                  1 {}  1\n     6 ROLLBACK INSERT                  1 {}  {}\n     7 REPLACE  {INSERT OR IGNORE}      0 {}  1\n     8 IGNORE   {INSERT OR REPLACE}     0 5   1\n     9 FAIL     {INSERT OR IGNORE}      0 {}  1\n    10 ABORT    {INSERT OR REPLACE}     0 5   1\n    11 ROLLBACK {INSERT OR IGNORE}      0 {}  1\n    12 {}       {INSERT OR IGNORE}      0 {}  1\n    13 {}       {INSERT OR REPLACE}     0 5   1\n    14 {}       {INSERT OR FAIL}        1 {}  1\n    15 {}       {INSERT OR ABORT}       1 {}  1\n    16 {}       {INSERT OR ROLLBACK}    1 {}  {}\n   "]]=]) do
        if t0
 then
            t1 = "NOT NULL constraint failed: t1.c"
        end
        test:do_test(
            "conflict-5."..i,
            function()
                if (conf1 ~= "")
 then
                    conf1 = "ON CONFLICT "..conf1..""
                end
                r0 = X(239, "X!cmd", [=[["catch","execsql [subst {\n         DROP TABLE t1;\n         CREATE TABLE t1(a,b,c NOT NULL $conf1 DEFAULT 5);\n         DELETE FROM t2;\n         BEGIN;\n         INSERT INTO t2 VALUES(1); \n         $cmd INTO t1 VALUES(1,2,NULL);\n       }]","r1"]]=])
                X(246, "X!cmd", [=[["catch","execsql {COMMIT}"]]=])
                if (not r0)
 then
                    r1 = test:execsql "SELECT c FROM t1"
                end
                r2 = test:execsql "SELECT x FROM t2"
                return { r0, r1, r2 }
            end, {
                t0, t1, t2
            })

    end
    test:do_execsql_test(
        "conflict-6.0",
        [[
            DROP TABLE t2;
            CREATE TABLE t2(a,b,c);
            INSERT INTO t2 VALUES(1,2,1);
            INSERT INTO t2 VALUES(2,3,2);
            INSERT INTO t2 VALUES(3,4,1);
            INSERT INTO t2 VALUES(4,5,4);
            SELECT c FROM t2 ORDER BY b;
            CREATE TABLE t3(x);
            INSERT INTO t3 VALUES(1);
        ]], {
            -- <conflict-6.0>
            1, 2, 1, 4
            -- </conflict-6.0>
        })

    -- Six columns of configuration data as follows:
    --
    --   i      The reference number of the test
    --   conf1  The conflict resolution algorithm on the UNIQUE constraint
    --   cmd    An UPDATE command to execute against table t1
    --   t0     True if there is an error from $cmd
    --   t1     Content of "b" column of t1 assuming no error in $cmd
    --   t2     Content of "x" column of t3
    --   t3     Number of temporary files for tables
    --   t4     Number of temporary files for statement journals
    --
    -- Update: Since temporary table files are now opened lazily, and none
    -- of the following tests use large quantities of data, t3 is always 0.
    --
    for _ in X(0, "X!foreach", [=[["i conf1 cmd t0 t1 t2 t3 t4","\n     1 {}       UPDATE                  1 {6 7 8 9}  1 0 1\n     2 REPLACE  UPDATE                  0 {7 6 9}    1 0 0\n     3 IGNORE   UPDATE                  0 {6 7 3 9}  1 0 0\n     4 FAIL     UPDATE                  1 {6 7 3 4}  1 0 0\n     5 ABORT    UPDATE                  1 {1 2 3 4}  1 0 1\n     6 ROLLBACK UPDATE                  1 {1 2 3 4}  0 0 0\n     7 REPLACE  {UPDATE OR IGNORE}      0 {6 7 3 9}  1 0 0\n     8 IGNORE   {UPDATE OR REPLACE}     0 {7 6 9}    1 0 0\n     9 FAIL     {UPDATE OR IGNORE}      0 {6 7 3 9}  1 0 0\n    10 ABORT    {UPDATE OR REPLACE}     0 {7 6 9}    1 0 0\n    11 ROLLBACK {UPDATE OR IGNORE}      0 {6 7 3 9}  1 0 0\n    12 {}       {UPDATE OR IGNORE}      0 {6 7 3 9}  1 0 0\n    13 {}       {UPDATE OR REPLACE}     0 {7 6 9}    1 0 0\n    14 {}       {UPDATE OR FAIL}        1 {6 7 3 4}  1 0 0\n    15 {}       {UPDATE OR ABORT}       1 {1 2 3 4}  1 0 1\n    16 {}       {UPDATE OR ROLLBACK}    1 {1 2 3 4}  0 0 0\n   "]]=]) do
        if t0
 then
            t1 = "UNIQUE constraint failed: t1.a"
        end
        if X(300, "X!cmd", [=[["expr","[info exists TEMP_STORE] && $TEMP_STORE==3"]]=])
 then
            t3 = 0
        else
            t3 = (t3 + t4)
        end
        test:do_test(
            "conflict-6."..i,
            function()
                db("close")
                sqlite3("db", "test.db")
                if (conf1 ~= "")
 then
                    conf1 = "ON CONFLICT "..conf1..""
                end
                test:execsql "pragma temp_store=file"
                sqlite_opentemp_count = 0
                r0 = X(312, "X!cmd", [=[["catch","execsql [subst {\n         DROP TABLE t1;\n         CREATE TABLE t1(a,b,c, UNIQUE(a) $conf1);\n         INSERT INTO t1 SELECT * FROM t2;\n         UPDATE t3 SET x=0;\n         BEGIN;\n         $cmd t3 SET x=1;\n         $cmd t1 SET b=b*2;\n         $cmd t1 SET a=c+5;\n       }]","r1"]]=])
                X(321, "X!cmd", [=[["catch","execsql {COMMIT}"]]=])
                if (not r0)
 then
                    r1 = test:execsql "SELECT a FROM t1 ORDER BY b"
                end
                r2 = test:execsql "SELECT x FROM t3"
                return { r0, r1, r2, sqlite_opentemp_count }
            end, {
                t0, t1, t2, t3
            })

    end
    -- Test to make sure a lot of IGNOREs don't cause a stack overflow
    --
    test:do_test(
        "conflict-7.1",
        function()
            test:execsql [[
                DROP TABLE t1;
                DROP TABLE t2;
                DROP TABLE t3;
                CREATE TABLE t1(a unique, b);
            ]]
            for _ in X(0, "X!for", [=[["set i 1","$i<=50","incr i"]]=]) do
                test:execsql(string.format("INSERT into t1 values(%s,%s);", i, (i + 1)))
            end
            return test:execsql [[
                SELECT count(*), min(a), max(b) FROM t1;
            ]]
        end, {
            -- <conflict-7.1>
            50, 1, 51
            -- </conflict-7.1>
        })

    test:do_execsql_test(
        "conflict-7.2",
        [[
            PRAGMA count_changes=on;
            UPDATE OR IGNORE t1 SET a=1000;
        ]], {
            -- <conflict-7.2>
            1
            -- </conflict-7.2>
        })

    test:do_test(
        "conflict-7.2.1",
        function()
            return db("changes")
        end, {
            -- <conflict-7.2.1>
            1
            -- </conflict-7.2.1>
        })

    test:do_execsql_test(
        "conflict-7.3",
        [[
            SELECT b FROM t1 WHERE a=1000;
        ]], {
            -- <conflict-7.3>
            2
            -- </conflict-7.3>
        })

    test:do_execsql_test(
        "conflict-7.4",
        [[
            SELECT count(*) FROM t1;
        ]], {
            -- <conflict-7.4>
            50
            -- </conflict-7.4>
        })

    test:do_execsql_test(
        "conflict-7.5",
        [[
            PRAGMA count_changes=on;
            UPDATE OR REPLACE t1 SET a=1001;
        ]], {
            -- <conflict-7.5>
            50
            -- </conflict-7.5>
        })

    test:do_test(
        "conflict-7.5.1",
        function()
            return db("changes")
        end, {
            -- <conflict-7.5.1>
            50
            -- </conflict-7.5.1>
        })

    test:do_execsql_test(
        "conflict-7.6",
        [[
            SELECT b FROM t1 WHERE a=1001;
        ]], {
            -- <conflict-7.6>
            51
            -- </conflict-7.6>
        })

    test:do_execsql_test(
        "conflict-7.7",
        [[
            SELECT count(*) FROM t1;
        ]], {
            -- <conflict-7.7>
            1
            -- </conflict-7.7>
        })

    -- Update for version 3: A SELECT statement no longer resets the change
    -- counter (Test result changes from 0 to 50).
    test:do_test(
        "conflict-7.7.1",
        function()
            return db("changes")
        end, {
            -- <conflict-7.7.1>
            50
            -- </conflict-7.7.1>
        })

    -- Make sure the row count is right for rows that are ignored on
    -- an insert.
    --
    test:do_test(
        "conflict-8.1",
        function()
            test:execsql [[
                DELETE FROM t1;
                INSERT INTO t1 VALUES(1,2);
            ]]
            return test:execsql [[
                INSERT OR IGNORE INTO t1 VALUES(2,3);
            ]]
        end, {
            -- <conflict-8.1>
            1
            -- </conflict-8.1>
        })

    test:do_test(
        "conflict-8.1.1",
        function()
            return db("changes")
        end, {
            -- <conflict-8.1.1>
            1
            -- </conflict-8.1.1>
        })

    test:do_execsql_test(
        "conflict-8.2",
        [[
            INSERT OR IGNORE INTO t1 VALUES(2,4);
        ]], {
            -- <conflict-8.2>
            0
            -- </conflict-8.2>
        })

    test:do_test(
        "conflict-8.2.1",
        function()
            return db("changes")
        end, {
            -- <conflict-8.2.1>
            0
            -- </conflict-8.2.1>
        })

    test:do_execsql_test(
        "conflict-8.3",
        [[
            INSERT OR REPLACE INTO t1 VALUES(2,4);
        ]], {
            -- <conflict-8.3>
            1
            -- </conflict-8.3>
        })

    test:do_test(
        "conflict-8.3.1",
        function()
            return db("changes")
        end, {
            -- <conflict-8.3.1>
            1
            -- </conflict-8.3.1>
        })

    test:do_execsql_test(
        "conflict-8.4",
        [[
            INSERT OR IGNORE INTO t1 SELECT * FROM t1;
        ]], {
            -- <conflict-8.4>
            0
            -- </conflict-8.4>
        })

    test:do_test(
        "conflict-8.4.1",
        function()
            return db("changes")
        end, {
            -- <conflict-8.4.1>
            0
            -- </conflict-8.4.1>
        })

    test:do_execsql_test(
        "conflict-8.5",
        [[
            INSERT OR IGNORE INTO t1 SELECT a+2,b+2 FROM t1;
        ]], {
            -- <conflict-8.5>
            2
            -- </conflict-8.5>
        })

    test:do_test(
        "conflict-8.5.1",
        function()
            return db("changes")
        end, {
            -- <conflict-8.5.1>
            2
            -- </conflict-8.5.1>
        })

    test:do_execsql_test(
        "conflict-8.6",
        [[
            INSERT OR IGNORE INTO t1 SELECT a+3,b+3 FROM t1;
        ]], {
            -- <conflict-8.6>
            3
            -- </conflict-8.6>
        })

    test:do_test(
        "conflict-8.6.1",
        function()
            return db("changes")
        end, {
            -- <conflict-8.6.1>
            3
            -- </conflict-8.6.1>
        })

    X(460, "X!cmd", [=[["integrity_check","conflict-8.99"]]=])
    test:do_execsql_test(
        "conflict-9.1",
        [[
            PRAGMA count_changes=0;
            CREATE TABLE t2(
              a INTEGER UNIQUE ON CONFLICT IGNORE,
              b INTEGER UNIQUE ON CONFLICT FAIL,
              c INTEGER UNIQUE ON CONFLICT REPLACE,
              d INTEGER UNIQUE ON CONFLICT ABORT,
              e INTEGER UNIQUE ON CONFLICT ROLLBACK
            );
            CREATE TABLE t3(x);
            INSERT INTO t3 VALUES(1);
            SELECT * FROM t3;
        ]], {
            -- <conflict-9.1>
            1
            -- </conflict-9.1>
        })

    test:do_catchsql_test(
        "conflict-9.2",
        [[
            INSERT INTO t2 VALUES(1,1,1,1,1);
            INSERT INTO t2 VALUES(2,2,2,2,2);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.2>
            0, {1, 1, 1, 1, 1, 2, 2, 2, 2, 2}
            -- </conflict-9.2>
        })

    test:do_catchsql_test(
        "conflict-9.3",
        [[
            INSERT INTO t2 VALUES(1,3,3,3,3);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.3>
            0, {1, 1, 1, 1, 1, 2, 2, 2, 2, 2}
            -- </conflict-9.3>
        })

    test:do_catchsql_test(
        "conflict-9.4",
        [[
            UPDATE t2 SET a=a+1 WHERE a=1;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.4>
            0, {1, 1, 1, 1, 1, 2, 2, 2, 2, 2}
            -- </conflict-9.4>
        })

    test:do_catchsql_test(
        "conflict-9.5",
        [[
            INSERT INTO t2 VALUES(3,1,3,3,3);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.5>
            1, "UNIQUE constraint failed: t2.b"
            -- </conflict-9.5>
        })

    test:do_catchsql_test(
        "conflict-9.6",
        [[
            UPDATE t2 SET b=b+1 WHERE b=1;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.6>
            1, "UNIQUE constraint failed: t2.b"
            -- </conflict-9.6>
        })

    test:do_catchsql_test(
        "conflict-9.7",
        [[
            BEGIN;
            UPDATE t3 SET x=x+1;
            INSERT INTO t2 VALUES(3,1,3,3,3);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.7>
            1, "UNIQUE constraint failed: t2.b"
            -- </conflict-9.7>
        })

    test:do_test(
        "conflict-9.8",
        function()
            test:execsql "COMMIT"
            return test:execsql "SELECT * FROM t3"
        end, {
            -- <conflict-9.8>
            2
            -- </conflict-9.8>
        })

    test:do_catchsql_test(
        "conflict-9.9",
        [[
            BEGIN;
            UPDATE t3 SET x=x+1;
            UPDATE t2 SET b=b+1 WHERE b=1;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.9>
            1, "UNIQUE constraint failed: t2.b"
            -- </conflict-9.9>
        })

    test:do_test(
        "conflict-9.10",
        function()
            test:execsql "COMMIT"
            return test:execsql "SELECT * FROM t3"
        end, {
            -- <conflict-9.10>
            3
            -- </conflict-9.10>
        })

    test:do_catchsql_test(
        "conflict-9.11",
        [[
            INSERT INTO t2 VALUES(3,3,3,1,3);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.11>
            1, "UNIQUE constraint failed: t2.d"
            -- </conflict-9.11>
        })

    test:do_catchsql_test(
        "conflict-9.12",
        [[
            UPDATE t2 SET d=d+1 WHERE d=1;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.12>
            1, "UNIQUE constraint failed: t2.d"
            -- </conflict-9.12>
        })

    test:do_catchsql_test(
        "conflict-9.13",
        [[
            BEGIN;
            UPDATE t3 SET x=x+1;
            INSERT INTO t2 VALUES(3,3,3,1,3);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.13>
            1, "UNIQUE constraint failed: t2.d"
            -- </conflict-9.13>
        })

    test:do_test(
        "conflict-9.14",
        function()
            test:execsql "COMMIT"
            return test:execsql "SELECT * FROM t3"
        end, {
            -- <conflict-9.14>
            4
            -- </conflict-9.14>
        })

    test:do_catchsql_test(
        "conflict-9.15",
        [[
            BEGIN;
            UPDATE t3 SET x=x+1;
            UPDATE t2 SET d=d+1 WHERE d=1;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.15>
            1, "UNIQUE constraint failed: t2.d"
            -- </conflict-9.15>
        })

    test:do_test(
        "conflict-9.16",
        function()
            test:execsql "COMMIT"
            return test:execsql "SELECT * FROM t3"
        end, {
            -- <conflict-9.16>
            5
            -- </conflict-9.16>
        })

    test:do_catchsql_test(
        "conflict-9.17",
        [[
            INSERT INTO t2 VALUES(3,3,3,3,1);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.17>
            1, "UNIQUE constraint failed: t2.e"
            -- </conflict-9.17>
        })

    test:do_catchsql_test(
        "conflict-9.18",
        [[
            UPDATE t2 SET e=e+1 WHERE e=1;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.18>
            1, "UNIQUE constraint failed: t2.e"
            -- </conflict-9.18>
        })

    test:do_catchsql_test(
        "conflict-9.19",
        [[
            BEGIN;
            UPDATE t3 SET x=x+1;
            INSERT INTO t2 VALUES(3,3,3,3,1);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.19>
            1, "UNIQUE constraint failed: t2.e"
            -- </conflict-9.19>
        })

    X(588, "X!cmd", [=[["verify_ex_errcode","conflict-9.21b","SQLITE_CONSTRAINT_UNIQUE"]]=])
    test:do_test(
        "conflict-9.20",
        function()
            X(591, "X!cmd", [=[["catch","execsql {COMMIT}"]]=])
            return test:execsql "SELECT * FROM t3"
        end, {
            -- <conflict-9.20>
            5
            -- </conflict-9.20>
        })

    test:do_catchsql_test(
        "conflict-9.21",
        [[
            BEGIN;
            UPDATE t3 SET x=x+1;
            UPDATE t2 SET e=e+1 WHERE e=1;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.21>
            1, "UNIQUE constraint failed: t2.e"
            -- </conflict-9.21>
        })

    X(601, "X!cmd", [=[["verify_ex_errcode","conflict-9.21b","SQLITE_CONSTRAINT_UNIQUE"]]=])
    test:do_test(
        "conflict-9.22",
        function()
            X(604, "X!cmd", [=[["catch","execsql {COMMIT}"]]=])
            return test:execsql "SELECT * FROM t3"
        end, {
            -- <conflict-9.22>
            5
            -- </conflict-9.22>
        })

    test:do_catchsql_test(
        "conflict-9.23",
        [[
            INSERT INTO t2 VALUES(3,3,1,3,3);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.23>
            0, {2, 2, 2, 2, 2, 3, 3, 1, 3, 3}
            -- </conflict-9.23>
        })

    test:do_catchsql_test(
        "conflict-9.24",
        [[
            UPDATE t2 SET c=c-1 WHERE c=2;
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.24>
            0, {2, 2, 1, 2, 2}
            -- </conflict-9.24>
        })

    test:do_catchsql_test(
        "conflict-9.25",
        [[
            BEGIN;
            UPDATE t3 SET x=x+1;
            INSERT INTO t2 VALUES(3,3,1,3,3);
            SELECT * FROM t2;
        ]], {
            -- <conflict-9.25>
            0, {3, 3, 1, 3, 3}
            -- </conflict-9.25>
        })

    test:do_test(
        "conflict-9.26",
        function()
            X(628, "X!cmd", [=[["catch","execsql {COMMIT}"]]=])
            return test:execsql "SELECT * FROM t3"
        end, {
            -- <conflict-9.26>
            6
            -- </conflict-9.26>
        })

    test:do_test(
        "conflict-10.1",
        function()
            test:catchsql [[
                DELETE FROM t1;
                BEGIN;
                INSERT OR ROLLBACK INTO t1 VALUES(1,2);
                INSERT OR ROLLBACK INTO t1 VALUES(1,3);
                COMMIT;
            ]]
            return test:execsql "SELECT * FROM t1"
        end, {
            -- <conflict-10.1>
            
            -- </conflict-10.1>
        })

    test:do_test(
        "conflict-10.2",
        function()
            test:catchsql [[
                CREATE TABLE t4(x);
                CREATE UNIQUE INDEX t4x ON t4(x);
                BEGIN;
                INSERT OR ROLLBACK INTO t4 VALUES(1);
                INSERT OR ROLLBACK INTO t4 VALUES(1);
                COMMIT;
            ]]
            return test:execsql "SELECT * FROM t4"
        end, {
            -- <conflict-10.2>
            
            -- </conflict-10.2>
        })

    -- Ticket #1171.  Make sure statement rollbacks do not
    -- damage the database.
    --
    test:do_test(
        "conflict-11.1",
        function()
            test:execsql [[
                -- Create a database object (pages 2, 3 of the file)
                BEGIN;
                  CREATE TABLE abc(a UNIQUE, b, c);
                  INSERT INTO abc VALUES(1, 2, 3);
                  INSERT INTO abc VALUES(4, 5, 6);
                  INSERT INTO abc VALUES(7, 8, 9);
                COMMIT;
            ]]
            -- Set a small cache size so that changes will spill into
            -- the database file.  
            test:execsql [[
                PRAGMA cache_size = 10;
            ]]
            -- Make lots of changes.  Because of the small cache, some
            -- (most?) of these changes will spill into the disk file.
            -- In other words, some of the changes will not be held in
            -- cache.
            --
            test:execsql [[
                BEGIN;
                  -- Make sure the pager is in EXCLUSIVE state.
                  CREATE TABLE def(d, e, f);
                  INSERT INTO def VALUES
                      ('xxxxxxxxxxxxxxx', 'yyyyyyyyyyyyyyyy', 'zzzzzzzzzzzzzzzz');
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  DELETE FROM abc WHERE a = 4;
            ]]
            -- Execute a statement that does a statement rollback due to
            -- a constraint failure.
            --
            test:catchsql [[
                INSERT INTO abc SELECT 10, 20, 30 FROM def;
            ]]
            -- Rollback the database.  Verify that the state of the ABC table
            -- is unchanged from the beginning of the transaction.  In other words,
            -- make sure the DELETE on table ABC that occurred within the transaction
            -- had no effect.
            --
            return test:execsql [[
                ROLLBACK;
                SELECT * FROM abc;
            ]]
        end, {
            -- <conflict-11.1>
            1, 2, 3, 4, 5, 6, 7, 8, 9
            -- </conflict-11.1>
        })

    X(712, "X!cmd", [=[["integrity_check","conflict-11.2"]]=])
    -- Repeat test conflict-11.1 but this time commit.
    --
    test:do_test(
        "conflict-11.3",
        function()
            test:execsql [[
                BEGIN;
                  -- Make sure the pager is in EXCLUSIVE state.
                  UPDATE abc SET a=a+1;
                  CREATE TABLE def(d, e, f);
                  INSERT INTO def VALUES
                      ('xxxxxxxxxxxxxxx', 'yyyyyyyyyyyyyyyy', 'zzzzzzzzzzzzzzzz');
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  DELETE FROM abc WHERE a = 4;
            ]]
            test:catchsql [[
                INSERT INTO abc SELECT 10, 20, 30 FROM def;
            ]]
            return test:execsql [[
                ROLLBACK;
                SELECT * FROM abc;
            ]]
        end, {
            -- <conflict-11.3>
            1, 2, 3, 4, 5, 6, 7, 8, 9
            -- </conflict-11.3>
        })

    -- Repeat test conflict-11.1 but this time commit.
    --
    test:do_test(
        "conflict-11.5",
        function()
            test:execsql [[
                BEGIN;
                  -- Make sure the pager is in EXCLUSIVE state.
                  CREATE TABLE def(d, e, f);
                  INSERT INTO def VALUES
                      ('xxxxxxxxxxxxxxx', 'yyyyyyyyyyyyyyyy', 'zzzzzzzzzzzzzzzz');
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  INSERT INTO def SELECT * FROM def;
                  DELETE FROM abc WHERE a = 4;
            ]]
            test:catchsql [[
                INSERT INTO abc SELECT 10, 20, 30 FROM def;
            ]]
            return test:execsql [[
                COMMIT;
                SELECT * FROM abc;
            ]]
        end, {
            -- <conflict-11.5>
            1, 2, 3, 7, 8, 9
            -- </conflict-11.5>
        })

    X(767, "X!cmd", [=[["integrity_check","conflict-11.6"]]=])
    -- Make sure UPDATE OR REPLACE works on tables that have only
    -- an INTEGER PRIMARY KEY.
    --
    test:do_execsql_test(
        "conflict-12.1",
        [[
            CREATE TABLE t5(a INTEGER PRIMARY KEY, b text);
            INSERT INTO t5 VALUES(1,'one');
            INSERT INTO t5 VALUES(2,'two');
            SELECT * FROM t5
        ]], {
            -- <conflict-12.1>
            1, "one", 2, "two"
            -- </conflict-12.1>
        })

    test:do_execsql_test(
        "conflict-12.2",
        [[
            UPDATE OR IGNORE t5 SET a=a+1 WHERE a=1;
            SELECT * FROM t5;
        ]], {
            -- <conflict-12.2>
            1, "one", 2, "two"
            -- </conflict-12.2>
        })

    test:do_catchsql_test(
        "conflict-12.3",
        [[
            UPDATE t5 SET a=a+1 WHERE a=1;
        ]], {
            -- <conflict-12.3>
            1, "UNIQUE constraint failed: t5.a"
            -- </conflict-12.3>
        })

    X(791, "X!cmd", [=[["verify_ex_errcode","conflict-12.3b","SQLITE_CONSTRAINT_PRIMARYKEY"]]=])
    test:do_execsql_test(
        "conflict-12.4",
        [[
            UPDATE OR REPLACE t5 SET a=a+1 WHERE a=1;
            SELECT * FROM t5;
        ]], {
            -- <conflict-12.4>
            2, "one"
            -- </conflict-12.4>
        })

    test:do_catchsql_test(
        "conflict-12.5",
        [[
            CREATE TABLE t5b(x);
            INSERT INTO t5b(rowid, x) VALUES(1,10),(2,11);
            UPDATE t5b SET rowid=rowid+1 WHERE x=10;
        ]], {
            -- <conflict-12.5>
            1, "UNIQUE constraint failed: t5b.rowid"
            -- </conflict-12.5>
        })

    X(805, "X!cmd", [=[["verify_ex_errcode","conflict-12.5b","SQLITE_CONSTRAINT_ROWID"]]=])
    -- Ticket [c38baa3d969eab7946dc50ba9d9b4f0057a19437]
    -- REPLACE works like ABORT on a CHECK constraint.
    --
    test:do_test(
        "conflict-13.1",
        function()
            test:execsql [[
                CREATE TABLE t13(a CHECK(a!=2));
                BEGIN;
                REPLACE INTO t13 VALUES(1);
            ]]
            return test:catchsql [[
                REPLACE INTO t13 VALUES(2);
            ]]
        end, {
            -- <conflict-13.1>
            1, "CHECK constraint failed: t13"
            -- </conflict-13.1>
        })

    X(821, "X!cmd", [=[["verify_ex_errcode","conflict-13.1b","SQLITE_CONSTRAINT_CHECK"]]=])
    test:do_execsql_test(
        "conflict-13.2",
        [[
            REPLACE INTO t13 VALUES(3);
            COMMIT;
            SELECT * FROM t13;
        ]], {
            -- <conflict-13.2>
            1, 3
            -- </conflict-13.2>
        })

end


test:finish_test()
