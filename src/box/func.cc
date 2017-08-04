/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "func.h"

#include "assoc.h"

#include <dlfcn.h>

#include "lua/utils.h"
#include "scoped_guard.h"

static struct mh_strnptr_t *modules = NULL;

/*
 * Return module struct and load corresponding dso if not loaded yet.
 */
static struct module *
module_load(const char *name)
{
	/*
	 * Module_load is the first function that first should be called
	 * if any module activity exists, so it is safe to check here
	 * modules hash table.
	 */
	if (modules == NULL && (modules = mh_strnptr_new()) == NULL) {
		tnt_error(OutOfMemory, sizeof(*modules), "malloc",
			  "modules hash table");
		return NULL;
	}

	size_t name_len = strlen(name);
	mh_int_t i = mh_strnptr_find_inp(modules, name, name_len);
	if (i != mh_end(modules))
		return (struct module *) mh_strnptr_node(modules, i)->val;
	struct module *module = (struct module *)malloc(sizeof(struct module));
	if (module == NULL) {
		tnt_error(OutOfMemory, sizeof(struct module), "malloc",
			  "struct module");
		return NULL;
	}
	module->funcs = 0;
	module->calls = 0;
	module->unloading = false;
	module->handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
	if (module->handle == NULL) {
		free(module);
		tnt_error(LoggedError, ER_LOAD_FUNCTION, name,
			  dlerror());
		return NULL;
	}

	uint32_t name_hash = mh_strn_hash(name, name_len);
	const struct mh_strnptr_node_t strnode = {
		name, name_len, name_hash, module};

	i = mh_strnptr_put(modules, &strnode, NULL, NULL);
	if (i == mh_end(modules)) {
		dlclose(module->handle);
		free(module);
		tnt_error(OutOfMemory, sizeof(strnode), "mh_strnptr_put",
			  "strnptr node");
		return NULL;
	}
	return module;
}

/*
 * Import a function from the module.
 */
static box_function_f
module_get(struct module *module, const char *name)
{
	box_function_f f = (box_function_f)dlsym(module->handle, name);
	if (f == NULL) {
		tnt_error(LoggedError, ER_LOAD_FUNCTION, name, dlerror());
		return NULL;
	}
	++module->funcs;
	return f;
}

/*
 * Check if module dso can be closed.
 */
static bool
module_check(struct module *module)
{
	if (module->unloading == false ||
	    module->funcs != 0 || module->calls != 0)
		return true;
	dlclose(module->handle);
	free(module);
	return false;
}

struct func *
func_new(struct func_def *def)
{
	struct func *func = (struct func *) malloc(sizeof(struct func));
	if (func == NULL) {
		diag_set(OutOfMemory, sizeof(*func), "malloc", "func");
		return NULL;
	}
	func->def = def;
	/** Nobody has access to the function but the owner. */
	memset(func->access, 0, sizeof(func->access));
	/*
	 * Do not initialize the privilege cache right away since
	 * when loading up a function definition during recovery,
	 * user cache may not be filled up yet (space _user is
	 * recovered after space _func), so no user cache entry
	 * may exist yet for such user.  The cache will be filled
	 * up on demand upon first access.
	 *
	 * Later on consistency of the cache is ensured by DDL
	 * checks (see user_has_data()).
	 */
	func->owner_credentials.auth_token = BOX_USER_MAX; /* invalid value */
	func->func = NULL;
	func->module = NULL;
	return func;
}

static void
func_unload(struct func *func)
{
	if (func->module) {
		--func->module->funcs;
		module_check(func->module);
	}
	func->module = NULL;
	func->func = NULL;
}

void
func_load(struct func *func)
{
	func_unload(func);

	struct lua_State *L = tarantool_L;
	int n = lua_gettop(L);

	auto l_guard = make_scoped_guard([=]{
		lua_settop(L, n);
	});
	/*
	 * Call package.searchpath(name, package.cpath) and use
	 * the path to the function in dlopen().
	 */
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "searchpath");

	/*
	 * Extract package name from function name.
	 * E.g. name = foo.bar.baz, function = baz, package = foo.bar
	 */
	const char *sym;
	const char *package = func->def->name;
	const char *package_end = strrchr(package, '.');
	if (package_end != NULL) {
		/* module.submodule.function => module.submodule, function */
		sym = package_end + 1;
	} else {
		/* package == function => function, function */
		sym = package;
		package_end = package + strlen(package);
	}

	/* First argument of searchpath: name */
	lua_pushlstring(L, package, package_end - package);
	/* Fetch  cpath from 'package' as the second argument */
	lua_getfield(L, -3, "cpath");

	if (lua_pcall(L, 2, 1, 0)) {
		tnt_raise(ClientError, ER_LOAD_FUNCTION, func->def->name,
			  lua_tostring(L, -1));
	}
	if (lua_isnil(L, -1)) {
		tnt_raise(ClientError, ER_LOAD_FUNCTION, func->def->name,
			  "shared library not found in the search path");
	}
	const char *module_name = lua_tostring(L, -1);
	struct module *module = module_load(module_name);
	if (module == NULL)
		diag_raise();
	func->func = module_get(module, sym);
	if (func->func == NULL)
		diag_raise();
	func->module = module;
}

void
func_update(struct func *func, struct func_def *def)
{
	func_unload(func);
	free(func->def);
	func->def = def;
}

void
func_delete(struct func *func)
{
	func_unload(func);
	free(func->def);
	free(func);
}
