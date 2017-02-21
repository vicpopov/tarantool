local fio = require('fio')
local log = require('log')

local utils               = require('tarantoolctl.utils')
local error               = utils.error
local is_callable         = utils.is_callable
local load_file_sandboxed = utils.load_file_sandboxed

local function tarantoolctl_cfg_new()
    local cfg = {
        values = {},
        defaults = {},
        formats = setmetatable({
            lua = function(self, default_path)
                local function load_dictionary(self, name, value)
                    -- assume, that dicts have only string keys (for now)
                    name = name == nil and '' or name .. '.'
                    for subname, subvalue in pairs(value) do
                        subname = name .. subname
                        if type(subvalue) == 'table' then
                            load_dictionary(self, subname, subvalue)
                        else
                            self.values[subname] = subvalue
                        end
                    end
                end

                local result_environment = {}
                load_file_sandboxed(default_path, result_environment, 'defaults')
                load_dictionary(self, nil, result_environment)
            end
        }, {
            __index = function(self, format)
                error('unknown config format "%s"', format)
            end
        })
    }
    return setmetatable(cfg, {
        __index = {
            register = function(self, name, opts)
                opts = table.deepcopy(opts or {})
                self.defaults[name] = opts
            end,
            get = function(self, name)
                local def_value = self.defaults[name]
                if self.values[name] == nil then
                    local default = def_value.default
                    if is_callable(default) then default = default() end
                    self.values[name] = default
                end
                local value = self.values[name]
                if value == nil and def_value.deprecated ~= nil then
                    value = self.values[def_value.deprecated]
                    if value ~= nil then
                        log.error('using deprecated value "%s" (now called "%s")',
                                  def_value.deprecated, name)
                    end
                end
                local tp = def_value.type
                if value ~= nil and type(value) ~= tp then
                    log.error('config "%s": expected type "%s", got "%s"',
                              name, tp, type(value))
                    return nil
                end
                return value
            end,
            load = function(self, default_path)
                if default_path == nil then
                    return
                end
                local bname = fio.basename(default_path)
                local ext = bname:split('.')
                local format = ext[#ext]
                if (bname:startswith('.') and #ext == 2) or #ext == 1 then
                    format = 'lua'
                end
                self.formats[format](self, default_path)
            end,
        },
    })
end

return {
    new = tarantoolctl_cfg_new,
}
