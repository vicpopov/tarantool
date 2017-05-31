test_run = require('test_run').new()

fiber = require 'fiber'
fio = require 'fio'
xlog = require 'xlog'

s = box.schema.space.create('test', {engine='vinyl'})
_ = s:create_index('pk')
s:replace{0}

-- According to the gh-2154, the vinyl do not store LSNs older
-- than the oldest vlsn. Lets create read view at the beginning
-- of the test to save LSNs in the vinyl. Else all of them will be
-- equal to 0 != snap_lsn.
-- To create the read view, start a fiber with the a transaction,
-- reading some key. And in the main fiber change the tuple with
-- the same key.
function read_view_f() box.begin() s:get{0} s:replace{0, 0} fiber.sleep(10000) box.commit() end
read_view = fiber.create(read_view_f)
s:replace{0, 0, 0} -- send to read view.

-- Start a few fibers populating the space in the background.
n_workers = 3
c = fiber.channel(n_workers)
test_run:cmd("setopt delimiter ';'")
for i=1,n_workers do
    fiber.create(function()
        for j=i,1000,n_workers do
            s:insert{j}
        end
        c:put(true)
    end)
end
test_run:cmd("setopt delimiter ''");

-- Let the background fibers run.
fiber.sleep(0.001)

-- Concurrent checkpoint.
box.snapshot()

-- Join background fibers.
for i=1,n_workers do c:get() end

read_view:wakeup()

-- Get list of files from the last checkpoint.
files = box.backup.start()

-- Extract the last checkpoint LSN and find
-- max LSN stored in run files.
snap_lsn = -1
run_lsn = -1
test_run:cmd("setopt delimiter ';'")
for _, path in ipairs(files) do
    suffix = string.gsub(path, '.*%.', '')
    if suffix == 'snap' then
        snap_lsn = tonumber(fio.basename(path, '.snap'))
    end
    if suffix == 'run' then
        for lsn, _ in xlog.pairs(path) do
            if run_lsn < lsn then run_lsn = lsn end
        end
    end
end
test_run:cmd("setopt delimiter ''");
snap_lsn >= 0
run_lsn >= 0

box.backup.stop()

-- Check that run files only contain statements
-- inserted before checkpoint.
snap_lsn == run_lsn or {snap_lsn, run_lsn}

s:drop()
