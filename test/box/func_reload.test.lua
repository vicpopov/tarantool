build_path = os.getenv("BUILDDIR")
package.cpath = build_path..'/test/box/?.so;'..build_path..'/test/box/?.dylib;'..package.cpath

net = require('net.box')
fiber = require('fiber')
c = net.connect(os.getenv("LISTEN"))

box.schema.func.create('reload.foo', {language = "C"})
box.schema.user.grant('guest', 'execute', 'function', 'reload.foo')
_ = box.schema.space.create('test')
_ = box.space.test:create_index('primary', {parts = {1, "integer"}})
box.schema.user.grant('guest', 'read,write', 'space', 'test')
os.execute("cd "..build_path.."/test/box;cp reload1.so reload.so")

--check not fail on non-load func
box.schema.func.reload("reload.foo")

-- test of usual case reload. No hanging calls
box.space.test:insert{0}
c:call("reload.foo", {1})
box.space.test:delete{0}
os.execute("cd "..build_path.."/test/box; rm reload.so; cp reload2.so reload.so")
box.schema.func.reload("reload.foo")
c:call("reload.foo")
box.space.test:select{}
box.space.test:truncate()

-- test case with hanging calls
os.execute("cd "..build_path.."/test/box; rm reload.so; cp reload1.so reload.so")
box.schema.func.reload("reload.foo")

fibers = 10
for i = 1, fibers do fiber.create(function() c:call("reload.foo", {i}) end) end

while box.space.test:count() < fibers do fiber.sleep(0.001) end
-- double reload doesn't fail waiting functions
box.schema.func.reload("reload.foo")

os.execute("cd "..build_path.."/test/box; rm reload.so; cp reload2.so reload.so")
box.schema.func.reload("reload.foo")
c:call("reload.foo")

while box.space.test:count() < 2 * fibers + 1 do fiber.sleep(0.001) end
box.space.test:select{}
box.schema.func.drop("reload.foo")
box.space.test:drop()
os.execute("cd "..build_path.."/test/box; rm reload.so")
