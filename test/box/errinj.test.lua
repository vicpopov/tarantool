errinj = box.error.injection
net_box = require('net.box')

space = box.schema.space.create('tweedledum')
index = space:create_index('primary', { type = 'hash' })

errinj.info()
errinj.set("some-injection", true)
errinj.set("some-injection") -- check error
space:select{222444}
errinj.set("ERRINJ_TESTING", true)
space:select{222444}
errinj.set("ERRINJ_TESTING", false)

-- Check how well we handle a failed log write
errinj.set("ERRINJ_WAL_IO", true)
space:insert{1}
space:get{1}
errinj.set("ERRINJ_WAL_IO", false)
space:insert{1}
errinj.set("ERRINJ_WAL_IO", true)
space:update(1, {{'=', 2, 2}})
space:get{1}
space:get{2}
errinj.set("ERRINJ_WAL_IO", false)
space:truncate()

-- Check a failed log rotation
errinj.set("ERRINJ_WAL_ROTATE", true)
space:insert{1}
space:get{1}
errinj.set("ERRINJ_WAL_ROTATE", false)
space:insert{1}
errinj.set("ERRINJ_WAL_ROTATE", true)
space:update(1, {{'=', 2, 2}})
space:get{1}
space:get{2}
errinj.set("ERRINJ_WAL_ROTATE", false)
space:update(1, {{'=', 2, 2}})
space:get{1}
space:get{2}
errinj.set("ERRINJ_WAL_ROTATE", true)
space:truncate()
errinj.set("ERRINJ_WAL_ROTATE", false)
space:truncate()

space:drop()

-- Check how well we handle a failed log write in DDL
s_disabled = box.schema.space.create('disabled')
s_withindex = box.schema.space.create('withindex')
index1 = s_withindex:create_index('primary', { type = 'hash' })
s_withdata = box.schema.space.create('withdata')
index2 = s_withdata:create_index('primary', { type = 'tree' })
s_withdata:insert{1, 2, 3, 4, 5}
s_withdata:insert{4, 5, 6, 7, 8}
index3 = s_withdata:create_index('secondary', { type = 'hash', parts = {2, 'unsigned', 3, 'unsigned' }})
errinj.set("ERRINJ_WAL_IO", true)
test = box.schema.space.create('test')
s_disabled:create_index('primary', { type = 'hash' })
s_disabled.enabled
s_disabled:insert{0}
s_withindex:create_index('secondary', { type = 'tree', parts = { 2, 'unsigned'} })
s_withindex.index.secondary
s_withdata.index.secondary:drop()
s_withdata.index.secondary.unique
s_withdata:drop()
box.space['withdata'].enabled
index4 = s_withdata:create_index('another', { type = 'tree', parts = { 5, 'unsigned' }, unique = false})
s_withdata.index.another
errinj.set("ERRINJ_WAL_IO", false)
test = box.schema.space.create('test')
index5 = s_disabled:create_index('primary', { type = 'hash' })
s_disabled.enabled
s_disabled:insert{0}
index6 = s_withindex:create_index('secondary', { type = 'tree', parts = { 2, 'unsigned'} })
s_withindex.index.secondary.unique
s_withdata.index.secondary:drop()
s_withdata.index.secondary
s_withdata:drop()
box.space['withdata']
index7 = s_withdata:create_index('another', { type = 'tree', parts = { 5, 'unsigned' }, unique = false})
s_withdata.index.another
test:drop()
s_disabled:drop()
s_withindex:drop()

-- Check transaction rollback when out of memory
env = require('test_run')
test_run = env.new()

s = box.schema.space.create('s')
_ = s:create_index('pk')
errinj.set("ERRINJ_TUPLE_ALLOC", true)
s:auto_increment{}
s:select{}
s:auto_increment{}
s:select{}
s:auto_increment{}
s:select{}
test_run:cmd("setopt delimiter ';'")
box.begin()
    s:insert{1}
box.commit();
box.rollback();
s:select{};
box.begin()
    s:insert{1}
    s:insert{2}
box.commit();
s:select{};
box.rollback();
box.begin()
    pcall(s.insert, s, {1})
    s:insert{2}
box.commit();
s:select{};
box.rollback();
errinj.set("ERRINJ_TUPLE_ALLOC", false);
box.begin()
    s:insert{1}
    errinj.set("ERRINJ_TUPLE_ALLOC", true)
    s:insert{2}
box.commit();
errinj.set("ERRINJ_TUPLE_ALLOC", false);
s:select{};
box.rollback();
s:select{};
box.begin()
    s:insert{1}
    errinj.set("ERRINJ_TUPLE_ALLOC", true)
    pcall(s.insert, s, {2})
box.commit();
s:select{};
box.rollback();

test_run:cmd("setopt delimiter ''");
errinj.set("ERRINJ_TUPLE_ALLOC", false)

s:drop()
s = box.schema.space.create('test')
_ = s:create_index('test', {parts = {1, 'unsigned', 3, 'unsigned', 5, 'unsigned'}})
s:insert{1, 2, 3, 4, 5, 6}
t = s:select{}[1]
errinj.set("ERRINJ_TUPLE_FIELD", true)
tostring(t[1]) .. tostring(t[2]) ..tostring(t[3]) .. tostring(t[4]) .. tostring(t[5]) .. tostring(t[6])
errinj.set("ERRINJ_TUPLE_FIELD", false)
tostring(t[1]) .. tostring(t[2]) ..tostring(t[3]) .. tostring(t[4]) .. tostring(t[5]) .. tostring(t[6])

s:drop()
s = box.schema.space.create('test')
_ = s:create_index('test', {parts = {2, 'unsigned', 4, 'unsigned', 6, 'unsigned'}})
s:insert{1, 2, 3, 4, 5, 6}
t = s:select{}[1]
errinj.set("ERRINJ_TUPLE_FIELD", true)
tostring(t[1]) .. tostring(t[2]) ..tostring(t[3]) .. tostring(t[4]) .. tostring(t[5]) .. tostring(t[6])
errinj.set("ERRINJ_TUPLE_FIELD", false)
tostring(t[1]) .. tostring(t[2]) ..tostring(t[3]) .. tostring(t[4]) .. tostring(t[5]) .. tostring(t[6])

-- Cleanup
s:drop()

--
-- gh-2046: don't store offsets for sequential multi-parts keys
--
s = box.schema.space.create('test')
_ = s:create_index('seq2', { parts = { 1, 'unsigned', 2, 'unsigned' }})
_ = s:create_index('seq3', { parts = { 1, 'unsigned', 2, 'unsigned', 3, 'unsigned' }})
_ = s:create_index('seq5', { parts = { 1, 'unsigned', 2, 'unsigned', 3, 'unsigned', 4, 'scalar', 5, 'number' }})
_ = s:create_index('rnd1', { parts = { 3, 'unsigned' }})

errinj.set("ERRINJ_TUPLE_FIELD", true)
tuple = s:insert({1, 2, 3, 4, 5, 6, 7, 8, 9, 10})
tuple
tuple[1] -- not-null, always accessible
tuple[2] -- null, doesn't have offset
tuple[3] -- not null, has offset
tuple[4] -- null, doesn't have offset
tuple[5] -- null, doesn't have offset
s.index.seq2:select({1})
s.index.seq2:select({1, 2})
s.index.seq3:select({1})
s.index.seq3:select({1, 2, 3})
s.index.seq5:select({1})
s.index.seq5:select({1, 2, 3, 4, 5})
s.index.rnd1:select({3})
errinj.set("ERRINJ_TUPLE_FIELD", false)
s:drop()

space = box.schema.space.create('test')
_ = space:create_index('pk')
errinj.set("ERRINJ_WAL_WRITE", true)
space:insert{1}
errinj.set("ERRINJ_WAL_WRITE", false)

errinj.set("ERRINJ_WAL_WRITE_DISK", true)
_ = space:insert{1, require'digest'.urandom(192 * 1024)}
errinj.set("ERRINJ_WAL_WRITE_DISK", false)
space:drop()

--test space:bsize() in case of memory error
utils = dofile('utils.lua')
s = box.schema.space.create('space_bsize')
idx = s:create_index('primary')

for i = 1, 13 do s:insert{ i, string.rep('x', i) } end

s:bsize()
utils.space_bsize(s)

errinj.set("ERRINJ_TUPLE_ALLOC", true)

s:replace{1, "test"}
s:bsize()
utils.space_bsize(s)

s:update({1}, {{'=', 3, '!'}})
s:bsize()
utils.space_bsize(s)

errinj.set("ERRINJ_TUPLE_ALLOC", false)

s:drop()

space = box.schema.space.create('test')
index1 = space:create_index('primary')
fiber = require'fiber'
ch = fiber.channel(1)

test_run:cmd('setopt delimiter ";"')
function test()
  errinj.set('ERRINJ_WAL_WRITE_DISK', true)
  pcall(box.space.test.replace, box.space.test, {1, 1})
  errinj.set('ERRINJ_WAL_WRITE_DISK', false)
  ch:put(true)
end ;

function run()
  fiber.create(test)
  box.snapshot()
end ;

test_run:cmd('setopt delimiter ""');

-- Port_dump can fail.

box.schema.user.grant('guest', 'read,write,execute', 'universe')

cn = net_box.connect(box.cfg.listen)
cn:ping()
errinj.set('ERRINJ_PORT_DUMP', true)
ok, ret = pcall(cn.space._space.select, cn.space._space)
assert(not ok)
assert(string.match(tostring(ret), 'Failed to allocate'))
errinj.set('ERRINJ_PORT_DUMP', false)
cn:close()
box.schema.user.revoke('guest', 'read, write, execute', 'universe')

run()
ch:get()

box.space.test:select()
test_run:cmd('restart server default')
box.space.test:select()
box.space.test:drop()

errinj = box.error.injection
net_box = require('net.box')
fiber = require'fiber'

s = box.schema.space.create('test')
_ = s:create_index('pk')

ch = fiber.channel(2)

test_run:cmd("setopt delimiter ';'")
function test(tuple)
   ch:put({pcall(s.replace, s, tuple)})
end;
test_run:cmd("setopt delimiter ''");

errinj.set("ERRINJ_WAL_WRITE", true)
_ = {fiber.create(test, {1, 2, 3}), fiber.create(test, {3, 4, 5})}

{ch:get(), ch:get()}
errinj.set("ERRINJ_WAL_WRITE", false)
s:drop()

-- rebuild some secondary indexes if the primary was changed
s = box.schema.space.create('test')
i1 = s:create_index('i1', {parts = {1, 'unsigned'}})
--i2 = s:create_index('i2', {parts = {5, 'unsigned'}, unique = false})
--i3 = s:create_index('i3', {parts = {6, 'unsigned'}, unique = false})
i2 = i1 i3 = i1

_ = s:insert{1, 4, 3, 4, 10, 10}
_ = s:insert{2, 3, 1, 2, 10, 10}
_ = s:insert{3, 2, 2, 1, 10, 10}
_ = s:insert{4, 1, 4, 3, 10, 10}


function sum(select_res) local r = 0 for _,t in pairs(select_res) do r = r + t[1] end return r end

i1:select{}
sum(i2:select{})
sum(i3:select{})

i1:alter({parts={2, 'unsigned'}})

_ = collectgarbage('collect')
i1:select{}
sum(i2:select{})
sum(i3:select{})

box.error.injection.set('ERRINJ_BUILD_SECONDARY', i2.id)

i1:alter{parts = {3, "unsigned"}}

_ = collectgarbage('collect')
i1:select{}
sum(i2:select{})
sum(i3:select{})

box.error.injection.set('ERRINJ_BUILD_SECONDARY', i3.id)

i1:alter{parts = {4, "unsigned"}}

_ = collectgarbage('collect')
i1:select{}
sum(i2:select{})
sum(i3:select{})

box.error.injection.set('ERRINJ_BUILD_SECONDARY', -1)

s:drop()
