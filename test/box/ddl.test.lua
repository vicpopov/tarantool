fiber = require 'fiber'
env = require('test_run')
test_run = env.new()

space = box.schema.space.create('test')
ch = fiber.channel(1)

test_run:cmd("setopt delimiter ';'")
function test()
    fiber.create(function () space:drop(space) ch:put(true) end)
    local status, res = pcall(space.create_index, space, 'pk')
    ch:get()
    return status, res
end

test_run:cmd("setopt delimiter ''");
test()

test_run:cmd('restart server default')

fiber = require 'fiber'
env = require('test_run')
test_run = env.new()

space = box.schema.space.create('test')
ind = space:create_index('pk')

space:replace({1, 2, 3})
space:replace({2, 3, 4})

ch = fiber.channel(1)
test_run:cmd("setopt delimiter ';'")
function test()
    box.space._space:update({space.id}, {{'=', 3, 'test1'}})
    ch:put(true)
end;

function state()
    fiber.create(test)
    space:replace({1, 5, 6})
    return {ind:select(),
        space:select(),
        space.name}
end;
test_run:cmd("setopt delimiter ''");

state()
ch:get()
ind:select()
space:select()
space.name

test_run:cmd('restart server default')

fiber = require'fiber'
env = require('test_run')
test_run = env.new()
errinj = box.error.injection

ch = fiber.channel(1)

space = box.space.test1
ind = space.index.pk

errinj.set("ERRINJ_WAL_WRITE", true)
test_run:cmd("setopt delimiter ';'")
function test()
    pcall(box.space._space.update, box.space._space, {space.id}, {{'=', 3, 'test'}})
    ch:put(true)
end;

function state()
    fiber.create(test)
    return {space:select(),
        ind:select(),
	space.name}
end;
test_run:cmd("setopt delimiter ''");

state()
ch:get()
-- space rolled back
space:select()
ind:select()
space.name

errinj.set("ERRINJ_WAL_WRITE", false)
space:drop()

