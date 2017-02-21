local fio      = require('fio')
local fun      = require('fun')
local log      = require('log')
local pwd      = require('pwd')
local argparse = require('internal.argparse').parse

local utils  = require('tarantoolctl.utils')
local config = require('tarantoolctl.config')

local error               = utils.error
local is_callable         = utils.is_callable
local execute_wrapped     = utils.execute_wrapped
local load_file_sandboxed = utils.load_file_sandboxed

--------------------------------------------------------------------------------
--                                 Constants                                  --
--------------------------------------------------------------------------------

-- '/etc/default/tarantool'
local DEFAULT_DEFAULTS_PATH = '@CMAKE_INSTALL_FULL_SYSCONFDIR@/@SYSCONFIG_DEFAULT@/tarantool'
local DEFAULT_WRAPPER_NAME  = 'tarantoolctl'
local ENV_PLUGIN_PATH       = 'TARANTOOLCTL_PLUGIN_PATH'

--------------------------------------------------------------------------------
--                        library/methods abstractions                        --
--------------------------------------------------------------------------------

local function usage_header()
    log.error("Tarantool client utility (%s)", _TARANTOOL)
    log.error("Usage:")
    log.error("")
end

-- trim whitespaces
local function string_trim(line)
    return line:gsub("^%s*(.-)%s*$", "%1")
end

-- split long line into muple one's with max width of 80 charachters and
-- prepends with depth spaces
local function print_aligned(lines, depth, print_f, multiline_indent)
    if lines == nil then
        return ''
    end
    local fields = table.concat(
        fun.iter(lines.split('\n')):map(function(line)
            line = string_trim(line)
            -- no space before punctuation
            local fs = line:byte(1)
            if fs ~= 44 and fs ~= 46 and fs ~= 58 and fs ~= 59 then
                line = ' ' .. line
            end
        end):totable(), ''
    )
    local first_line = true
    while true do
        if #lines == 0 then break end
        local line = nil
        if #lines <= 80 then
            line = lines
        else
            line = lines:sub(0, 80 - depth + 1):match("(.*%s)[^%s]*")
        end
        if not line or #line == 0 then
            line = lines:sub(0, 80 - depth) .. '-'
        end
        lines = lines:sub(#line + 1)

        line = string_trim(line);
        line = line:rjust(depth + #line)
        if multiline_indent then
            if first_line then
                first_line = false
            else
                line = '    ' .. line
            end
        end
        print_f(line)
    end
    return fields
end

local tctl_method_methods = {
    run = function(self, context)
        log.verbose("calling callback '%s'", self.name)
        local stat, rv = execute_wrapped(self.callback, context)
        if rv == 'usage' then
            return self:usage()
        end
        if self.opts.exiting or rv == false then
            os.exit(rv and 0 or 1)
        end
        return rv
    end,
    usage = function(self, opts)
        local tctl = getmetatable(self).__tarantoolctl
        opts = opts or {}
        opts.depth = opts.depth

        local header = self.help.header
        if tctl.linkmode then
            if not self.help.linkmode then
                return
            end
            header = self.help.linkmode
        end
        if type(header) ~= 'table' then
            header = { header }
        end
        for _, line in ipairs(header) do
            line = line:format(tctl.program_name)
            print_aligned(line, opts.depth, log.info, true)
        end

        opts.detailed = opts.detailed or tctl.help
        if opts.detailed then
            local description = self.help.description
            log.info("")
            print_aligned(description, opts.depth + 2, log.info, false)
            log.info("")
            if self.help.arguments then
                for _, arg in ipairs(self.help.arguments) do
                    log.info(arg[1]:rjust(#arg[1] + opts.depth + 2))
                    print_aligned(arg[2], opts.depth + 4, log.info, false)
                end
                log.info("")
            end
        end
        return false
    end,
    plugin_api = function(self)
        return self
    end,
    public_api = function(self)
        return self
    end
}

local function tctl_method_new(name, callback, opts, tctl)
    -- checks must be here
    local help = opts.help
    opts.help = nil
    return setmetatable({
        name     = name,
        callback = callback,
        opts     = opts,
        help     = help
    }, {
        __index        = tctl_method_methods,
        __tarantoolctl = tctl
    })
end

local tctl_library_plugin_methods = {
    register_method  = function(self, name, callback, opts)
        log.verbose("registering method '%s' for library '%s'", name, self.name)
        opts = table.deepcopy(opts or {})
        opts.help = opts.help or {}
        assert(type(opts.help.header) == 'string' or
               type(opts.help.header) == 'table')
        assert(type(opts.help.description) == 'string')
        if not opts.help.weight then
            opts.help.weight = fun.iter(self.methods):map(function(_, val)
                return val.help.weight
            end):chain({ 0 }):max() + 10
        end
        if self.methods[name] ~= nil then
            error('Method "%s" exists in "%s" library', name, self.name)
        end

        local meth_instance = tctl_method_new(name, callback, opts,
                                              getmetatable(self).__tarantoolctl)
        self.methods[name] = meth_instance

        return meth_instance:plugin_api()
    end,
    register_prepare = function(self, name, callback)
        log.verbose("registering context prepare function '%s'", name)
        if not is_callable(callback) then
            error('prepare function "%s" is not callable', name)
        end
        table.insert(self.prepare, {name, callback})
    end
}

local tctl_library_methods = {
    plugin_api = function(self)
        return setmetatable(fun.iter(self):tomap(), {
            __index = tctl_library_plugin_methods,
            __tarantoolctl = getmetatable(self).__tarantoolctl
        })
    end,
    public_api = function(self)
        return self
    end,
    return_sorted = function(self)
        local sorted = fun.iter(self.methods):map(function(name, val)
            return {val.help.weight or 0, name}
        end):totable();
        table.sort(sorted, function(l, r) return l[1] < r[1] end)
        return fun.iter(sorted):map(function(value)
            return self.methods[value[2]]
        end):totable()
    end,
    usage = function(self, opts)
        local tctl = getmetatable(self).__tarantoolctl
        opts = opts or {}
        opts.depth    = opts.depth    or 0
        opts.detailed = opts.detailed or tctl.help
        local nested = opts.nested
        local depth  = opts.depth

        if tctl.linkmode then
            local have_linkmode = false
            for name, method in pairs(self.methods) do
                if method.help.linkmode then
                    have_linkmode = true
                    break
                end
            end
            if not have_linkmode then
                if not opts.nested then
                    log.error("%s library doesn't support link mode", self.name)
                end
                return false
            end
        end

        if self.command == nil then
            if nested then
                log.info("%s[%s library]", string.rep(' ', depth), self.name)
            else
                log.error("Expected command name, got nothing")
            end
        elseif self.methods[self.command] == nil then
            log.error("Command '%s' isn't found in module '%s'", self.command,
                      self.name)
        end
        if not nested then
            log.error("")
            usage_header()
        end
        opts.depth = opts.depth + 4
        for _, val in ipairs(self:return_sorted()) do
            val:usage(opts)
            opts.first = true
        end
        opts.depth = opts.depth - 4
        return false
    end,
    run = function(self)
        local tctl = getmetatable(self).__tarantoolctl
        self.command = table.remove(self.arguments, 1)
        if self.command == nil then
            return self:usage()
        end
        local wrapper = self.methods[self.command]
        if wrapper == nil or tctl.help then
            return self:usage()
        end
        do -- prepare context here
            self.context.command = self.command
            self.context.positional_arguments = {}
            self.context.keyword_arguments    = {}
            for k, v in pairs(tctl.arguments) do
                if type(k) == 'number' then
                    self.context.positional_arguments[k] = v
                else
                    self.context.keyword_arguments[k] = v
                end
            end
            for _, prepare in ipairs(self.prepare) do
                local name, cb = unpack(prepare)
                log.verbose("running context prepare function '%s'", name)
                if cb(tctl:public_api(), self.context) == false then
                    return self:usage()
                end
            end
        end
        return wrapper:run(self.context)
    end,
}

local function tctl_library_new(name, opts, tctl)
    local help = opts.help
    opts.help = nil
    return setmetatable({
        name        = name,
        command     = nil,
        methods     = {},
        prepare     = {},
        context     = {},
        arguments   = tctl.arguments,
        opts        = opts,
        help        = help
    }, {
        __index = tctl_library_methods,
        __tarantoolctl = tctl
    })
end

local function find_defaults_file_user()
    local user = pwd.getpw()
    log.verbose('user with id "%s" is used', user.id)
    if user.id ~= 0 then
        -- check in current directory
        local defaults = fio.pathjoin(fio.cwd(), '.tarantoolctl')
        log.verbose('defaults file: trying to find "%s"', defaults)
        if fio.stat(defaults) then
            return true, defaults
        end
        -- check in home directory
        defaults = user.workdir
        if defaults ~= nil then
            defaults = fio.pathjoin(defaults, '.config/tarantool/tarantool')
            log.verbose('defaults file: trying to find "%s"', defaults)
            if fio.stat(defaults) then
                return true, defaults
            end
        end
    end
    -- we weren't been able to found tarantoolctl config in local/home
    -- directories (or we're 'root')
    return false, nil
end

local function find_defaults_file()
    -- try to find local/user configuration
    local user, defaults = find_defaults_file_user()
    if user == false then
        -- try to find system-wide configuration
        defaults = DEFAULT_DEFAULTS_PATH
        log.verbose('defaults file: trying to find "%s"', defaults)
        if not fio.stat(defaults) then
            defaults = nil
        end
    end

    if defaults == nil then
        log.verbose("can't find defaults file.")
    else
        log.verbose('using "%s" as defaults file', defaults)
    end
    -- no defaults path, assume defaults
    return user, defaults
end

local tctl_plugin_methods = {
    register_library = function(self, name, opts)
        log.verbose("registering library '%s'", name)
        opts = table.deepcopy(opts or {})
        opts.help = opts.help or {}
        if self.libraries[name] ~= nil then
            log.error("failed to register library. already exists")
            return nil
        end
        if not opts.help.weight then
            opts.help.weight = fun.iter(self.libraries):map(function(_, val)
                return val.help.weight
            end):chain({ 0 }):max() + 10
        end

        local lib_instance = tctl_library_new(name, opts,
                                              getmetatable(self).__tarantoolctl)
        self.libraries[name] = lib_instance

        return lib_instance:plugin_api()
    end,
    register_alias   = function(self, name, dotted_path, cfg)
        log.verbose("registering alias '%s' to '%s'", name, dotted_path)
        local path = dotted_path:split('.')
        local lname, mname = unpack(path)
        if #path ~= 2 then
            log.error("bad alias path '%s' (expected 2 components, got %d)",
                      dotted_path, #path)
            return nil
        end
        local library = self.libraries[lname]
        if library == nil then
            log.error("bad alias path '%s' (library '%s' not found)",
                      dotted_path, lname)
            return nil
        end
        local method = library.methods[mname]
        if method == nil then
            log.error("bad alias path '%s' (method '%s' not found)",
                      dotted_path, lname)
            return nil
        end

        cfg = cfg or {}

        self.aliases[name] = {
            path = dotted_path,
            deprecated = cfg.deprecated or false
        }

        return method:plugin_api()
    end,
    register_config  = function(self, name, opts)
        log.verbose("registering configuration value '%s'", name)
        if opts.deprecated then
            log.verbose("deprecated name: %s", opts.deprecated)
        end
        if opts.type then
            log.verbose("expected type:   %s", opts.type)
        end
        if opts.default then
            log.verbose("default value:   %s", opts.default)
        end
        self.cfg:register(name, opts)
    end
}

local tctl_public_methods = {
    get_config = function(self, name)
        log.verbose("getting configuration value for '%s'", name)
        return self.cfg:get(name)
    end
}

local tctl_methods = {
    load_defaults = function(self)
        self.usermode, self.defaults = find_defaults_file()
        self.cfg:load(self.defaults)
        return self
    end,
    load_plugin = function(self, n, file, count)
        local function plugin_count_len(n, count)
            local cnt = #tostring(count)
            return string.format('%0'.. cnt .. 'd/%0' .. cnt .. 'd', n, count)
        end

        file = fio.abspath(file)
        log.verbose("loading plugin %s '%s'", plugin_count_len(n, count), file)
        load_file_sandboxed(file, {}, 'plugins')
    end,
    load_plugin_list = function(self, plugins)
        local plugin_cnt = #plugins
        log.verbose("found %d plugin files", plugin_cnt)
        for n, file in ipairs(plugins) do
            self:load_plugin(n, file, plugin_cnt)
        end
    end,
    load_plugin_directory = function(self, plugin_dir_path)
        log.verbose("loading plugins from '%s'", plugin_dir_path)

        if fio.stat(plugin_dir_path) == nil then
            log.verbose("failed to open path '%s'", plugin_dir_path)
            return nil
        end

        local re_plugins = fio.pathjoin(plugin_dir_path, '*.lua')
        local plugins = fio.glob(re_plugins)

        return self:load_plugin_list(plugins)
    end,
    load_plugin_package_path_patterns = function(self)
        log.verbose("loading plugins from package.path")

        local path_pattern = package.path:gsub('?', '*/tarantoolctl')
        local pattern_list = {}
        for pattern in path_pattern:gmatch('([^;]+);') do
            table.insert(pattern_list, pattern)
        end
        local plugins = {}
        for _, re in ipairs(pattern_list) do
            log.verbose("matching pattern '%s'", re)
            local matched_plugins = fio.glob(re)
            for _, name in ipairs(matched_plugins) do
                log.verbose("found plugin: '%s'", name)
                table.insert(plugins, name)
            end
        end

        return self:load_plugin_list(plugins)
    end,
    plugin_api = function(self)
        return setmetatable(fun.iter(self):tomap(), {
            __index        = tctl_plugin_methods,
            __tarantoolctl = self
        })
    end,
    load_plugins = function(self)
        package.loaded['ctl'] = self:plugin_api()
        local plugin_path = os.getenv(ENV_PLUGIN_PATH)
        if plugin_path ~= nil then
            self:load_plugin_directory(plugin_path)
        end
        self:load_plugin_package_path_patterns()
        package.loaded['ctl'] = nil
        return self
    end,
    public_api = function(self)
        return setmetatable(fun.iter(self):tomap(), {
            __index = tctl_public_methods,
        })
    end,
    usage = function(self, opts)
        opts = opts or {}
        opts.detailed = opts.detailed or false
        opts.depth    = opts.depth    or 0
        opts.header   = opts.header   or false

        if self.command ~= nil then
            log.error("Unknown library or command name '%s'", self.command)
            log.error("")
        end

        usage_header()

        local sorted = fun.iter(self.libraries):map(function(name, val)
            return {val.help.weight or 0, name}
        end):totable();

        table.sort(sorted, function(l, r) return l[1] < r[1] end)
        opts.depth = opts.depth + 4
        opts.nested = true
        local lsorted = #sorted
        fun.iter(sorted):enumerate():each(function(n, val)
            local rv = self.libraries[val[2]]:usage(opts)
            if rv ~= nil then log.info("") end
        end)
        opts.depth = opts.depth - 4
        return false
    end,
    run = function(self)
        self.command = table.remove(self.arguments, 1)
        if self.command == nil then
            return self:usage()
        end
        log.verbose('running %s', self.command)
        local alias = self.aliases[self.command]
        if alias ~= nil then
            local path = alias.path:split('.')
            table.insert(self.arguments, 1, path[2])
            self.command = path[1]
        end
        local wrapper = self.libraries[self.command]
        if wrapper == nil then
            return self:usage()
        end
        return wrapper:run()
    end,
}

local function is_linkmode(program_name)
    return not (fio.basename(program_name, '.lua') == DEFAULT_WRAPPER_NAME) and
           not (fio.basename(program_name, '.lua') == 'binary')
end

local tarantoolctl_singleton = nil

local function tarantoolctl_new()
    if tarantoolctl_singleton == nil then
        local tctl = setmetatable({
            libraries  = {},
            aliases    = {},
            plugins    = {},
            cfg        = config.new(),
            -- defaults, make copy of them, to modify them later
            help         = false,
            linkmode     = nil,
            executable   = arg[-1],
            program_name = arg[ 0],
            arguments    = argparse(arg),
            verbosity    = 0,
            exiting      = false
        }, {
            __index = tctl_methods
        })

        -- make copy of arguments for modification
        local arg = tctl.arguments
        tctl.linkmode  = is_linkmode(tctl.program_name)
        tctl.help      = arg.h or arg.help or false
        -- we shouldn't throw errors until this place.
        -- output before that point is buggy
        tctl.verbosity = #(type(arg.v) ~= 'table' and {arg.v} or arg.v)
        if tctl.verbosity > 0 then
            log.level(6)
        end
        tarantoolctl_singleton = tctl:plugin_api()
        return tctl
    end
    return tarantoolctl_singleton
end

return {
    new = tarantoolctl_new,
}
