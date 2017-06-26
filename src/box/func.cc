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

#include <dlfcn.h>

#include "lua/utils.h"
#include "scoped_guard.h"

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
	func->dlhandle = NULL;
	func->state = NOT_LOADED;
	func->active_calls = 0;
	return func;
}

static void
func_unload(struct func *func)
{
	/* should not be called if there is active call */
	if (func->active_calls > 0) {
		return;
	}
	if (func->dlhandle)
		dlclose(func->dlhandle);
	func->dlhandle = NULL;
	func->func = NULL;
	func->state = NOT_LOADED;
	func->active_calls = 0;
}


const char *
find_package(struct func *func)
{
	/*
	 * Call package.searchpath(name, package.cpath) and use
	 * the path to the function in dlopen().
	 */
	struct lua_State *L = tarantool_L;
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
	return sym;
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
	const char *sym = find_package(func);

	func->dlhandle = dlopen(lua_tostring(L, -1), RTLD_NOW | RTLD_LOCAL);
	if (func->dlhandle == NULL) {
		tnt_raise(LoggedError, ER_LOAD_FUNCTION, func->def->name,
			  dlerror());
	}
	func->func = (box_function_f) dlsym(func->dlhandle, sym);
	if (func->func == NULL) {
		tnt_raise(LoggedError, ER_LOAD_FUNCTION, func->def->name,
			  dlerror());
	}
	func->state = LOADED;
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
	/* to be deleted after return from call */
	func->func = NULL;
	if (func->active_calls > 0) {
		/* should not free memory if there is active call */
		return;
	}
	func_unload(func);
	if (func->def != NULL)
		free(func->def);
	func->def = NULL;
	free(func);
}

struct func *
func_reload(struct func *func)
{
	/**
	 * dlopen forbids reloading new version of library
	 * if it's opened. It resolves reopening by name.
	 * We can't close running library, because there may be
	 * waiting calls and after close, they will fail.
	 * Therefore we create hardlink to the library file
	 * and create new a new copy object of struct func
	 * with proper opened dlhandle and func fields.
	 */
	struct lua_State *L = tarantool_L;
	const char *sym = find_package(func);
	const char *package_name = lua_tostring(L, -1);
	char path[PATH_MAX  +1];
	if (dlinfo(func->dlhandle, RTLD_DI_ORIGIN, path) < 0) {
		tnt_raise(LoggedError, ER_LOAD_FUNCTION, func->def->name,
			  dlerror());
	}
	char tmp_name[PATH_MAX + 1];
	if (tmpnam(tmp_name) == NULL) {
		tnt_raise(SystemError, "failed to create unique filename");
	}
	/**
	 * Size of new name is len of path and len of tmp name excluding "tmp/"
	 * and including \0
	 */
	size_t name_len = strlen(path) + strlen(tmp_name) - 4 + 1;
	char *new_name = (char *) malloc(name_len);
	if (new_name == NULL) {
		tnt_raise(OutOfMemory, name_len, "malloc", "new_name");
	}
	snprintf(new_name, name_len, "%s%s", path, tmp_name + 4);
	if (link(package_name, new_name) < 0) {
		free(new_name);
		tnt_raise(SystemError, "failed to create link error:%s",
				strerror(errno));
	}


	struct func *new_func = func_new(func->def);
	auto l_guard = make_scoped_guard([=]{
		/* cleanup only in case if error */
		if (new_func->func == NULL) {
			func_delete(new_func);
			free(new_name);
			}
	});

	/* copy new object of function */
	new_func->def = (struct func_def *)
		malloc(func_def_sizeof(strlen(func->def->name)));
	if (new_func->def == NULL) {
		tnt_raise(OutOfMemory,
				func_def_sizeof(strlen(func->def->name)),
				"malloc", "def");
	}
	*new_func->def = *func->def;
	snprintf(new_func->def->name, strlen(func->def->name) + 1, "%s",
			func->def->name);
	new_func->owner_credentials = func->owner_credentials;
	memcpy(new_func->access, func->access, BOX_USER_MAX);
	/* open new version of library */
	new_func->dlhandle = dlopen(new_name, RTLD_NOW | RTLD_LOCAL);
	unlink(new_name);
	if (new_func->dlhandle == NULL) {
		tnt_raise(LoggedError, ER_LOAD_FUNCTION, func->def->name,
			  dlerror());
	}
	new_func->func = (box_function_f) dlsym(new_func->dlhandle, sym);
	if (new_func->func == NULL) {
		tnt_raise(LoggedError, ER_LOAD_FUNCTION, func->def->name,
			  dlerror());
	}
	new_func->state = LOADED;

	/* try to clean up */
	func_delete(func);
	free(new_name);
	return new_func;
}
