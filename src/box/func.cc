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

#include <stdio.h>
#include <dlfcn.h>

#include "assoc.h"

#include "lua/utils.h"
#include "scoped_guard.h"

static struct mh_strnptr_t *modules = NULL;

/*
 * Split function name to module and symbol names.
 */
static void
split_names(const char *name, const char **module_name,
	    const char **module_name_end, const char **sym)
{
	*module_name = name;
	*module_name_end = strrchr(name, '.');
	if (*module_name_end != NULL) {
		/* module.submodule.function => module.submodule, function */
		*sym = *module_name_end + 1;
	} else {
		/* module == function => function, function */
		*sym = name;
		*module_name_end = name + strlen(name);
	}
}

/*
 * Create modules hash table if needed.
 */
static inline int
modules_check()
{
	if (modules == NULL && (modules = mh_strnptr_new()) == NULL) {
		tnt_error(OutOfMemory, sizeof(*modules), "malloc",
			  "modules hash table");
		return -1;
	}
	return 0;
}

/*
 * Save module to modules hash table.
 */
static inline mh_int_t
module_put(const char *name, size_t name_len, struct module *module)
{
	uint32_t name_hash = mh_strn_hash(name, name_len);
	const struct mh_strnptr_node_t strnode = {
		name, name_len, name_hash, module};

	return mh_strnptr_put(modules, &strnode, NULL, NULL);
}

/*
 * Load a dso.
 * Create a new symlink based on temporary directory and try to
 * load via this symink to load a dso twice for cases of a function
 * reload.
 */
static struct module *
module_create(const char *name)
{
	struct module *module = (struct module *)malloc(sizeof(struct module));
	if (module == NULL) {
		tnt_error(OutOfMemory, sizeof(struct module), "malloc",
			  "struct module");
		return NULL;
	}
	rlist_create(&module->funcs);
	module->calls = 0;
	module->unloading = false;
	char load_name[PATH_MAX + 1];
	if (tmpnam(load_name) == NULL) {
		tnt_error(SystemError, "failed to create unique filename");
		return NULL;
	}
	if (symlink(name, load_name) < 0) {
		tnt_error(SystemError, "failed to create dso link");
		return NULL;
	}
	module->handle = dlopen(load_name, RTLD_NOW | RTLD_LOCAL);
	if (unlink(load_name) < 0) {
		say_warn("Failed to unlink dso link %s", load_name);
	}
	if (module->handle == NULL) {
		free(module);
		tnt_error(LoggedError, ER_LOAD_FUNCTION, name,
			  dlerror());
		return NULL;
	}
	return module;
}

static void
module_destroy(struct module *module)
{
	dlclose(module->handle);
	free(module);
}

/*
 * Return module struct and load corresponding dso if it is not loaded yet.
 */
static struct module *
module_load(const char *name)
{
	if (modules_check() != 0)
		return NULL;
	size_t name_len = strlen(name);
	mh_int_t i = mh_strnptr_find_inp(modules, name, name_len);
	if (i != mh_end(modules))
		return (struct module *)mh_strnptr_node(modules, i)->val;

	struct module *module = module_create(name);
	if (module == NULL)
		return NULL;

	if (module_put(name, name_len, module) == mh_end(modules))
		return NULL;

	return module;
}

/*
 * Import a function from the module.
 */
static box_function_f
module_sym(struct module *module, const char *name)
{
	box_function_f f = (box_function_f)dlsym(module->handle, name);
	if (f == NULL) {
		tnt_error(LoggedError, ER_LOAD_FUNCTION, name, dlerror());
		return NULL;
	}
	return f;
}

/*
 * Check if a dso is unused and can be closed.
 */
static bool
module_check(struct module *module)
{
	if (module->unloading == false ||
	    rlist_empty(&module->funcs) == false || module->calls != 0)
		return true;
	module_destroy(module);
	return false;
}

/*
 * Reload a dso.
 */
struct module *
module_reload(const char *name)
{
	if (modules_check() != 0)
		return NULL;

	size_t name_len = strlen(name);
	mh_int_t i = mh_strnptr_find_inp(modules, name, name_len);
	if (i == mh_end(modules)) {
		/* Module wasn't loaded, just load it. */
		return module_load(name);
	}
	struct module *old_module = (struct module *)mh_strnptr_node(modules, i)->val;


	struct module *new_module = module_create(name);
	if (new_module == NULL)
		return NULL;

	struct func *func, *tmp_func;
	rlist_foreach_entry_safe(func, &old_module->funcs, item, tmp_func)
	{
		const char *module_name, *module_name_end, *sym;
		split_names(func->def->name, &module_name,
			    &module_name_end, &sym);
		func->func = module_sym(new_module, sym);
		if (func->func == NULL)
			goto restore;
		func->module = new_module;
		rlist_move(&new_module->funcs, &func->item);
	}
	old_module->unloading = true;
	module_put(name, strlen(name), new_module);
	module_check(old_module);
	return new_module;
restore:
	/*
	 * Some old-dso func can't be load from new module, restore old
	 * functions.
	 */
	do {
		func->func = module_sym(old_module, func->def->name);
		if (func->func == NULL) {
			/*
			 * Something strange was happen, an early loaden
			 * function was not found in an old dso.
			 */
			panic("Can't restore module function, "
			      "server state is inconsistent");
		}
		func->module = old_module;
		rlist_move(&old_module->funcs, &func->item);
	} while (func != rlist_first_entry(&old_module->funcs,
					   struct func, item));
	module_destroy(new_module);
	return NULL;
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
		rlist_del(&func->item);
		module_check(func->module);
	}
	func->module = NULL;
	func->func = NULL;
}

/*
 * Find module filename.
 */
static const char *
module_find(const char *module_name, const char *module_name_end)
{
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

	/* First argument of searchpath: name */
	lua_pushlstring(L, module_name, module_name_end - module_name);
	/* Fetch  cpath from 'package' as the second argument */
	lua_getfield(L, -3, "cpath");

	if (lua_pcall(L, 2, 1, 0)) {
		tnt_raise(ClientError, ER_LOAD_FUNCTION, *module_name,
			  lua_tostring(L, -1));
	}
	if (lua_isnil(L, -1)) {
		tnt_raise(ClientError, ER_LOAD_FUNCTION, *module_name,
			  "shared library not found in the search path");
	}
	return lua_tostring(L, -1);
}

void
func_load(struct func *func)
{
	func_unload(func);

	const char *module_name, *module_name_end, *sym;
	split_names(func->def->name, &module_name, &module_name_end, &sym);
	module_name = module_find(module_name, module_name_end);

	struct module *module = module_load(module_name);
	if (module == NULL)
		diag_raise();
	func->func = module_sym(module, sym);
	if (func->func == NULL)
		diag_raise();
	func->module = module;
	rlist_add(&module->funcs, &func->item);
}

void
func_reload(struct func *func)
{
	const char *module_name, *module_name_end, *sym;
	split_names(func->def->name, &module_name, &module_name_end, &sym);
	module_name = module_find(module_name, module_name_end);
	struct module *module = module_reload(module_name);
	if (module == NULL) {
		error_log(diag_last_error(diag_get()));
		luaT_error(tarantool_L);
	}
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

int
func_call(struct func *func, box_function_ctx_t *ctx,
	  const char *args, const char *args_end)
{
	/* Module can be changed after function reload. */
	struct module *module = func->module;
	if (module != NULL)
		++module->calls;
	int rc = func->func(ctx, args, args_end);
	if (module != NULL)
		--module->calls;
	module_check(module);
	return rc;
}
