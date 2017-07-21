test_run = require('test_run').new()

--
-- Standalone sequences.
--

-- Options check on create.
box.schema.sequence.create('test', {step = 'a'})
box.schema.sequence.create('test', {min = 'b'})
box.schema.sequence.create('test', {max = 'c'})
box.schema.sequence.create('test', {start = true})
box.schema.sequence.create('test', {cycle = 123})
box.schema.sequence.create('test', {name = 'test'})
box.schema.sequence.create('test', {space_id = 512})
box.schema.sequence.create('test', {step = 0})
box.schema.sequence.create('test', {min = 10, max = 1})
box.schema.sequence.create('test', {min = 10, max = 20, start = 1})

-- Options check on alter.
_ = box.schema.sequence.create('test')
box.schema.sequence.alter('test', {step = 'a'})
box.schema.sequence.alter('test', {min = 'b'})
box.schema.sequence.alter('test', {max = 'c'})
box.schema.sequence.alter('test', {start = true})
box.schema.sequence.alter('test', {cycle = 123})
box.schema.sequence.alter('test', {name = 'test'})
box.schema.sequence.alter('test', {space_id = 512})
box.schema.sequence.alter('test', {if_not_exists = false})
box.schema.sequence.alter('test', {step = 0})
box.schema.sequence.alter('test', {min = 10, max = 1})
box.schema.sequence.alter('test', {min = 10, max = 20, start = 1})
box.schema.sequence.drop('test')

-- Duplicate name.
sq1 = box.schema.sequence.create('test')
box.schema.sequence.create('test')
sq2, msg = box.schema.sequence.create('test', {if_not_exists = true})
sq1 == sq2, msg
_ = box.schema.sequence.create('test2')
box.schema.sequence.alter('test2', {name = 'test'})
box.schema.sequence.drop('test2')
box.schema.sequence.drop('test')

-- Check that box.sequence gets updated.
sq = box.schema.sequence.create('test')
box.sequence.test == sq
sq:alter{step = 2}
box.sequence.test == sq
sq:drop()
box.sequence.test == nil

-- Default ascending sequence.
sq = box.schema.sequence.create('test')
sq.step, sq.min, sq.max, sq.start, sq.cycle
sq:next()
sq:next()
sq:set(100)
sq:next()
sq:next()
sq:reset()
sq:next()
sq:next()
sq:drop()

-- Default descending sequence.
sq = box.schema.sequence.create('test', {step = -1})
sq.step, sq.min, sq.max, sq.start, sq.cycle
sq:next()
sq:next()
sq:set(-100)
sq:next()
sq:next()
sq:reset()
sq:next()
sq:next()
sq:drop()

-- Custom min/max.
sq = box.schema.sequence.create('test', {min = 10})
sq.step, sq.min, sq.max, sq.start, sq.cycle
sq:next()
sq:next()
sq:drop()
sq = box.schema.sequence.create('test', {step = -1, max = 20})
sq.step, sq.min, sq.max, sq.start, sq.cycle
sq:next()
sq:next()
sq:drop()

-- Custom start value.
sq = box.schema.sequence.create('test', {start = 1000})
sq.step, sq.min, sq.max, sq.start, sq.cycle
sq:next()
sq:next()
sq:reset()
sq:next()
sq:next()
sq:drop()

-- Overflow and cycle.
sq = box.schema.sequence.create('test', {max = 2})
sq:next()
sq:next()
sq:next()
sq:alter{cycle = true}
sq:next()
sq:next()
sq:next()
sq:alter{step = 2}
sq:next()
sq:alter{cycle = false}
sq:next()
sq:drop()

-- Setting sequence value outside boundaries.
sq = box.schema.sequence.create('test')

sq:alter{step = 1, min = 1, max = 10}
sq:set(-100)
sq:next()
sq:set(100)
sq:next()
sq:reset()
sq:next()
sq:alter{min = 5, start = 5}
sq:next()
sq:reset()

sq:alter{step = -1, min = 1, max = 10, start = 10}
sq:set(100)
sq:next()
sq:set(-100)
sq:next()
sq:reset()
sq:next()
sq:alter{max = 5, start = 5}
sq:next()
sq:drop()

-- number64 arguments.
INT64_MIN = tonumber64('-9223372036854775808')
INT64_MAX = tonumber64('9223372036854775807')
sq = box.schema.sequence.create('test', {step = INT64_MAX, min = INT64_MIN, max = INT64_MAX, start = INT64_MIN})
sq:next()
sq:next()
sq:next()
sq:next()
sq:alter{step = INT64_MIN, start = INT64_MAX}
sq:reset()
sq:next()
sq:next()
sq:next()
sq:drop()

-- Using in a transaction.
s = box.schema.space.create('test')
_ = s:create_index('pk')
sq1 = box.schema.sequence.create('sq1', {step = 1})
sq2 = box.schema.sequence.create('sq2', {step = -1})

test_run:cmd("setopt delimiter ';'")
box.begin()
s:insert{sq1:next(), sq2:next()}
s:insert{sq1:next(), sq2:next()}
s:insert{sq1:next(), sq2:next()}
box.commit()
test_run:cmd("setopt delimiter ''");

s:select()

sq1:drop()
sq2:drop()
s:drop()

--
-- Space auto increment sequence.
--

-- Check that only primary key over the first field of type
-- 'unsigned' can be auto incremented.
s = box.schema.space.create('test')
s:create_index('pk', {parts = {2, 'unsigned'}, auto_increment = true})
s:create_index('pk', {parts = {1, 'unsigned', 2, 'string'}, auto_increment = true})
_ = s:create_index('pk', {parts = {1, 'unsigned'}, auto_increment = true})
s:create_index('sk', {parts = {1, 'unsigned'}, auto_increment = true})
_ = s:create_index('sk', {parts = {2, 'string'}})
sq = box.sequence._auto_increment_test
sq.space_id == s.id
sq.step, sq.min, sq.max, sq.start

-- Auto increment sequence can't be altered.
box.sequence._auto_increment_test:alter{step = -1}

-- Drop of the primary key or space deletes the space sequence.
s.index.sk:drop()
box.sequence._auto_increment_test ~= nil
s.index.pk:drop()
box.sequence._auto_increment_test == nil
_ = s:create_index('pk', {parts = {1, 'unsigned'}, auto_increment = true})
_ = s:create_index('sk', {parts = {2, 'string'}})
box.sequence._auto_increment_test ~= nil
s:drop()
box.sequence._auto_increment_test == nil

-- Check that inserting a nil value to the primary key triggers auto-increment.
s = box.schema.space.create('test')
_ = s:create_index('pk', {auto_increment = true})
s:insert(box.tuple.new(nil))

-- Check that inserting an empty tuple fails as expected.
s:insert{}

-- Check that deletion of the max key doesn't break the sequence.
s:delete{1}
s:replace{nil, 'a'}
s:select()

-- Check that insertion of a key > sequence value updates the sequence.
s:insert{10, 'b'}
s:insert{nil, 'c'}
s:select()

-- Check that the legacy auto-increment still works and doesn't break
-- the sequence.
s:auto_increment{'d'}
s:insert{nil, 'e'}
s:select()

-- Check that auto-increment works in a transaction.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:delete{13}
s:insert{nil, 'e'}
s:replace{nil, 'f'}
box.commit()
test_run:cmd("setopt delimiter ''");
s:select()

s:drop()

-- Check that auto-increment does not work for a space without a sequence.
s = box.schema.space.create('test')
_ = s:create_index('pk')
s:insert(box.tuple.new(nil))
s:insert{nil, 1}
s:insert{1, 1}
s:select()
s:drop()

--
-- Check that sequences are persistent.
--

sq1 = box.schema.sequence.create('sq1', {step = 2, min = 10, max = 20, start = 15, cycle = true})
sq2 = box.schema.sequence.create('sq2', {step = -2, min = -10, max = -1, start = -5})
sq1:next()

s1 = box.schema.space.create('test1')
_ = s1:create_index('pk', {auto_increment = true})

s1:insert{nil, 1}
s1:insert{nil, 2}
s1:insert{nil, 3}
s1:delete{3}

s2 = box.schema.space.create('test2')
_ = s2:create_index('pk', {auto_increment = true})

test_run:cmd('restart server default')

sq1 = box.sequence.sq1
sq2 = box.sequence.sq2

s1 = box.space.test1
s2 = box.space.test2
box.sequence._auto_increment_test1 ~= nil
box.sequence._auto_increment_test2 ~= nil

sq1:next()
sq2:next()
s1:insert(box.tuple.new(nil))
s2:insert(box.tuple.new(nil))

s1:select()
s2:select()

sq1:drop()
sq2:drop()
s1:drop()
s2:drop()
