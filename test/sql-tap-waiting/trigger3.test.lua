#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(17)

--!./tcltestrunner.lua
-- The author disclaims copyright to this source code.  In place of
-- a legal notice, here is a blessing:
--
--    May you do good and not evil.
--    May you find forgiveness for yourself and forgive others.
--    May you share freely, never taking more than you give.
--
-------------------------------------------------------------------------
--
-- This file tests the RAISE() function.
--
-- ["set","testdir",[["file","dirname",["argv0"]]]]
-- ["source",[["testdir"],"\/tester.tcl"]]


-- The tests in this file were written before SQLite supported recursive }
-- trigger invocation, and some tests depend on that to pass. So disable
-- recursive triggers for this file.
test:catchsql " pragma recursive_triggers = off "
-- MUST_WORK_TEST
if (1 > 0) then
    -- Test that we can cause ROLLBACK, FAIL and ABORT correctly
    test:catchsql " CREATE TABLE tbl(a primary key, b ,c) "
    test:execsql [[
        CREATE TRIGGER before_tbl_insert BEFORE INSERT ON tbl BEGIN SELECT CASE
            WHEN (new.a = 4) THEN RAISE(IGNORE) END;
        END;

        CREATE TRIGGER after_tbl_insert AFTER INSERT ON tbl BEGIN SELECT CASE
            WHEN (new.a = 1) THEN RAISE(ABORT,    'Trigger abort')
            WHEN (new.a = 2) THEN RAISE(FAIL,     'Trigger fail')
            WHEN (new.a = 3) THEN RAISE(ROLLBACK, 'Trigger rollback') END;
        END;
    ]]
    -- ABORT
    test:do_catchsql_test(
        "trigger3-1.1",
        [[
            BEGIN;
            INSERT INTO tbl VALUES (5, 5, 6);
            INSERT INTO tbl VALUES (1, 5, 6);
        ]], {
            -- <trigger3-1.1>
            1, "Trigger abort"
            -- </trigger3-1.1>
        })

    -- verify_ex_errcode trigger3-1.1b SQLITE_CONSTRAINT_TRIGGER
    test:do_execsql_test(
        "trigger3-1.2",
        [[
            SELECT * FROM tbl;
            ROLLBACK;
        ]], {
            -- <trigger3-1.2>
            5, 5, 6
            -- </trigger3-1.2>
        })

    test:do_execsql_test(
        "trigger3-1.3",
        [[
            SELECT * FROM tbl
        ]], {
            -- <trigger3-1.3>
            
            -- </trigger3-1.3>
        })

    -- FAIL
    test:do_catchsql_test(
        "trigger3-2.1",
        [[
            BEGIN;
            INSERT INTO tbl VALUES (5, 5, 6);
            INSERT INTO tbl VALUES (2, 5, 6);
        ]], {
            -- <trigger3-2.1>
            1, "Trigger fail"
            -- </trigger3-2.1>
        })

    -- verify_ex_errcode trigger3-2.1b SQLITE_CONSTRAINT_TRIGGER
    test:do_execsql_test(
        "trigger3-2.2",
        [[
            SELECT * FROM tbl;
            ROLLBACK;
        ]], {
            -- <trigger3-2.2>
            5, 5, 6, 2, 5, 6
            -- </trigger3-2.2>
        })

    -- ROLLBACK
    test:do_catchsql_test(
        "trigger3-3.1",
        [[
            BEGIN;
            INSERT INTO tbl VALUES (5, 5, 6);
            INSERT INTO tbl VALUES (3, 5, 6);
        ]], {
            -- <trigger3-3.1>
            1, "Trigger rollback"
            -- </trigger3-3.1>
        })

    -- verify_ex_errcode trigger3-3.1b SQLITE_CONSTRAINT_TRIGGER
    test:do_execsql_test(
        "trigger3-3.2",
        [[
            SELECT * FROM tbl;
        ]], {
            -- <trigger3-3.2>
            
            -- </trigger3-3.2>
        })

    -- Verify that a ROLLBACK trigger works like a FAIL trigger if
    -- we are not within a transaction.  Ticket #3035.
    --
    test:do_test(
        "trigger3-3.3",
        function()
            test:catchsql "COMMIT"
            return test:catchsql [[
                INSERT INTO tbl VALUES (3, 9, 10);
            ]]
        end, {
            -- <trigger3-3.3>
            1, "Trigger rollback"
            -- </trigger3-3.3>
        })

    -- verify_ex_errcode trigger3-3.3b SQLITE_CONSTRAINT_TRIGGER
    test:do_execsql_test(
        "trigger3-3.4",
        [[
            SELECT * FROM tbl
        ]], {
            -- <trigger3-3.4>
            
            -- </trigger3-3.4>
        })

    -- IGNORE
    test:do_catchsql_test(
        "trigger3-4.1",
        [[
            BEGIN;
            INSERT INTO tbl VALUES (5, 5, 6);
            INSERT INTO tbl VALUES (4, 5, 6);
        ]], {
            -- <trigger3-4.1>
            0, {}
            -- </trigger3-4.1>
        })

    test:do_execsql_test(
        "trigger3-4.2",
        [[
            SELECT * FROM tbl;
            ROLLBACK;
        ]], {
            -- <trigger3-4.2>
            5, 5, 6
            -- </trigger3-4.2>
        })

    -- Check that we can also do RAISE(IGNORE) for UPDATE and DELETE
    test:execsql "DROP TABLE tbl;"
    test:execsql "CREATE TABLE tbl (a primary key, b, c);"
    test:execsql "INSERT INTO tbl VALUES(1, 2, 3);"
    test:execsql "INSERT INTO tbl VALUES(4, 5, 6);"
    test:execsql [[
        CREATE TRIGGER before_tbl_update BEFORE UPDATE ON tbl BEGIN
            SELECT CASE WHEN (old.a = 1) THEN RAISE(IGNORE) END;
        END;

        CREATE TRIGGER before_tbl_delete BEFORE DELETE ON tbl BEGIN
            SELECT CASE WHEN (old.a = 1) THEN RAISE(IGNORE) END;
        END;
    ]]
    test:do_execsql_test(
        "trigger3-5.1",
        [[
            UPDATE tbl SET c = 10;
            SELECT * FROM tbl;
        ]], {
            -- <trigger3-5.1>
            1, 2, 3, 4, 5, 10
            -- </trigger3-5.1>
        })

    test:do_execsql_test(
        "trigger3-5.2",
        [[
            DELETE FROM tbl;
            SELECT * FROM tbl;
        ]], {
            -- <trigger3-5.2>
            1, 2, 3
            -- </trigger3-5.2>
        })

    -- Check that RAISE(IGNORE) works correctly for nested triggers:
    test:execsql "CREATE TABLE tbl2(a primary key, b, c)"
    test:execsql [[
        CREATE TRIGGER after_tbl2_insert AFTER INSERT ON tbl2 BEGIN
            UPDATE tbl SET c = 10;
            INSERT INTO tbl2 VALUES (new.a, new.b, new.c);
        END;
    ]]
    test:do_execsql_test(
        "trigger3-6",
        [[
            INSERT INTO tbl2 VALUES (1, 2, 3);
            SELECT * FROM tbl2;
            SELECT * FROM tbl;
        ]], {
            -- <trigger3-6>
            1, 2, 3, 1, 2, 3, 1, 2, 3
            -- </trigger3-6>
        })

    -- Check that things also work for view-triggers
    test:execsql "CREATE VIEW tbl_view AS SELECT * FROM tbl"
    test:execsql [[
        CREATE TRIGGER tbl_view_insert INSTEAD OF INSERT ON tbl_view BEGIN
            SELECT CASE WHEN (new.a = 1) THEN RAISE(ROLLBACK, 'View rollback')
                        WHEN (new.a = 2) THEN RAISE(IGNORE)
                        WHEN (new.a = 3) THEN RAISE(ABORT, 'View abort') END;
        END;
    ]]
    test:do_catchsql_test(
        "trigger3-7.1",
        [[
            INSERT INTO tbl_view VALUES(1, 2, 3);
        ]], {
            -- <trigger3-7.1>
            1, "View rollback"
            -- </trigger3-7.1>
        })

    -- verify_ex_errcode trigger3-7.1b SQLITE_CONSTRAINT_TRIGGER
    test:do_catchsql_test(
        "trigger3-7.2",
        [[
            INSERT INTO tbl_view VALUES(2, 2, 3);
        ]], {
            -- <trigger3-7.2>
            0, {}
            -- </trigger3-7.2>
        })

    test:do_catchsql_test(
        "trigger3-7.3",
        [[
            INSERT INTO tbl_view VALUES(3, 2, 3);
        ]], {
            -- <trigger3-7.3>
            1, "View abort"
            -- </trigger3-7.3>
        })

    -- verify_ex_errcode trigger3-7.3b SQLITE_CONSTRAINT_TRIGGER


    -- ifcapable view
    -- integrity_check trigger3-8.1
    test:catchsql " DROP TABLE tbl; "
    test:catchsql " DROP TABLE tbl2; "
    test:catchsql " DROP VIEW tbl_view; "
end


test:finish_test()
