local log   = require('log')
local fio   = require('fio')
local errno = require('errno')

local function log_traceback(level, ldepth)
    local function get_traceback(ldepth, level)
        local tb = {}
        local level = 2 + (ldepth or 1)
        while true do
            local info = debug.getinfo(level)
            assert(type(info) == 'nil' or type(info) == 'table')
            if info == nil then
                break
            end
            table.insert(tb, {
                line = info.currentline or 0,
                what = info.what or 'undef',
                file = info.short_src or info.src or 'eval',
                name = info.name,
            })
            level = level + 1
        end
        return tb
    end
    level = level and log[level] or log.verbose
    for _, fr in ipairs(get_traceback(ldepth)) do
        local name = ''
        if fr.name ~= nil then
            name = (" function '%s'"):format(fr.name)
        end
        level("[%-4s]%s at <%s:%d>", fr.what, name, fr.file, fr.line)
    end
end

local function log_syserror(fmt, ...)
    if select('#', ...) > 0 then
        fmt = fmt:format(...)
    end
    log.error(('[errno %d] %s: %s'):format(errno(), fmt, errno.strerror()))
end

local g_error = _G.error

local function error(...)
    local fmt_pos, level = 2, 2
    local fmt = ...
    if type(fmt) == 'number' then
        fmt_pos = 3
        level, fmt = ...
    end
    -- format error message
    local stat = true
    if select('#', ...) >= fmt_pos then
        stat, fmt = pcall(string.format, select(fmt_pos - 1, ...))
    end
    g_error(fmt, stat == false and 2 or level)
end

local function syserror(...)
    local fmt_pos, level = 2, 2
    local fmt = ...
    if type(fmt) == 'number' then
        fmt_pos = 3
        level, fmt = ...
    end
    -- format error message
    fmt = ('[errno %d] %s: %s'):format(fmt, errno.strerror())
    local stat, fmt = pcall(string.format, fmt, errno(), select(fmt_pos, ...))
    g_error(fmt, stat == false and 2 or level)
end

local function execute_wrapped(func, ...)
    local function xpcall_traceback_callback(err)
        if err == 'usage' then
            return err
        end
        err = err or '<none>'
        if type(err) == 'cdata' then
            err = tostring(err)
        end
        local err_place = nil
        if err:match(':%d+: ') then
            err_place, err = err:match('(.+:%d+): (.+)')
        end
        log.error('Error catched: %s', err)
        if err_place ~= nil then
            log.error("Error occured at '%s'", err_place)
        end
        log.error('')
        log_traceback('error', 2)
        return err
    end

    return xpcall(func, xpcall_traceback_callback, ...)
end

local function is_callable(arg)
    if arg ~= nil then
        local mt = (type(arg) == 'table' and getmetatable(arg) or nil)
        if type(arg) == 'function' or mt ~= nil and
           type(mt.__call) == 'function' then
            return true
        end
    end
    return false
end

local function load_file_sandboxed(path, env, desc)
    path = fio.abspath(path)
    local ufunc, msg = loadfile(path)
    if not ufunc then
        log.error("Failed to load %s file '%s':", desc, path)
        log.error(msg)
        return false
    end
    debug.setfenv(ufunc, setmetatable(env, { __index = _G }))
    local rval = { execute_wrapped(ufunc) }
    if not rval[1] then
        log.error("Failed to execute %s file '%s':", desc, path)
        log.error(rval[2])
        return false
    end
    table.remove(rval, 1)
    return unpack(rval)
end

local function load_func_sandboxed(ufunc, env, desc)
    debug.setfenv(ufunc, setmetatable(env, { __index = _G }))
    local rval = { execute_wrapped(ufunc) }
    if not rval[1] then
        log.error("Failed to execute '%s' function:", desc)
        log.error(rval[2])
        return false
    end
    table.remove(rval, 1)
    return unpack(rval)
end

return {
    error               = error,
    syserror            = syserror,
    log_syserror        = log_syserror,
    log_traceback       = log_traceback,
    execute_wrapped     = execute_wrapped,
    is_callable         = is_callable,
    load_file_sandboxed = load_file_sandboxed,
    load_func_sandboxed = load_func_sandboxed,
}
