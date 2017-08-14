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

/**
 * Parsed symbol and package names.
 */
struct func_name {
	/** Null-terminated symbol name, e.g. "func" for "mod.submod.func" */
	const char *sym;
	/** Package name, e.g. "mod.submod" for "mod.submod.func" */
	const char *package;
	/** A pointer to the last character in ->package + 1 */
	const char *package_end;
};

/***
 * Split function name to symbol and package names.
 * For example, str = foo.bar.baz => sym = baz, package = foo.bar
 * @param str function name, e.g. "module.submodule.function".
 * @param[out] name parsed symbol and package names.
 */
static void
func_split_name(const char *str, struct func_name *name)
{
	name->package = str;
	name->package_end = strrchr(str, '.');
	if (name->package_end != NULL) {
		/* module.submodule.function => module.submodule, function */
		name->sym = name->package_end + 1; /* skip '.' */
	} else {
		/* package == function => function, function */
		name->sym = name->package;
		name->package_end = str + strlen(str);
	}
}

/**
 * Arguments for luaT_module_find used by lua_cpcall()
 */
struct module_find_ctx {
	const char *package;
	const char *package_end;
	char *path;
	size_t path_len;
};

/**
 * A cpcall() helper for module_find()
 */
static int
luaT_module_find(lua_State *L)
{
	struct module_find_ctx *ctx = (struct module_find_ctx *)
		lua_topointer(L, 1);

	/*
	 * Call package.searchpath(name, package.cpath) and use
	 * the path to the function in dlopen().
	 */
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "searchpath");

	/* First argument of searchpath: name */
	lua_pushlstring(L, ctx->package, ctx->package_end - ctx->package);
	/* Fetch  cpath from 'package' as the second argument */
	lua_getfield(L, -3, "cpath");

	lua_call(L, 2, 1);
	if (lua_isnil(L, -1))
		return luaL_error(L, "module not found in package.cpath");

	snprintf(ctx->path, ctx->path_len, "%s", lua_tostring(L, -1));
	return 0;
}

/**
 * Find path to module using Lua's package.cpath
 * @param package package name
 * @param package_end a pointer to the last byte in @a package + 1
 * @param[out] path path to shared library
 * @param path_len size of @a path buffer
 * @retval 0 on success
 * @retval -1 on error, diag is set
 */
static int
module_find(const char *package, const char *package_end, char *path,
	    size_t path_len)
{
	struct module_find_ctx ctx = { package, package_end, path, path_len };
	lua_State *L = tarantool_L;
	int top = lua_gettop(L);
	if (luaT_cpcall(L, luaT_module_find, &ctx) != 0) {
		int package_len = (int) (package_end - package);
		diag_set(ClientError, ER_LOAD_MODULE, package_len, package,
			 lua_tostring(L, -1));
		lua_settop(L, top);
		return -1;
	}
	assert(top == lua_gettop(L)); /* cpcall discard results */
	return 0;
}

static struct mh_strnptr_t *modules = NULL;

static void
module_gc(struct module *module);

int
module_init(void)
{
	modules = mh_strnptr_new();
	if (modules == NULL) {
		diag_set(OutOfMemory, sizeof(*modules), "malloc",
			  "modules hash table");
		return -1;
	}
	return 0;
}

void
module_free(void)
{
	while (mh_size(modules) > 0) {
		mh_int_t i = mh_first(modules);
		struct module *module =
			(struct module *) mh_strnptr_node(modules, i)->val;
		/* Can't delete modules if they have active calls */
		module_gc(module);
	}
	mh_strnptr_delete(modules);
}

/**
 * Look up a module in the modules cache.
 */
static struct module *
module_cache_find(const char *name, const char *name_end)
{
	mh_int_t i = mh_strnptr_find_inp(modules, name, name_end - name);
	if (i == mh_end(modules))
		return NULL;
	return (struct module *)mh_strnptr_node(modules, i)->val;
}

/**
 * Save module to the module cache.
 */
static inline int
module_cache_put(const char *name, const char *name_end, struct module *module)
{
	size_t name_len = name_end - name;
	uint32_t name_hash = mh_strn_hash(name, name_len);
	const struct mh_strnptr_node_t strnode = {
		name, name_len, name_hash, module};

	if (mh_strnptr_put(modules, &strnode, NULL, NULL) == mh_end(modules)) {
		diag_set(OutOfMemory, sizeof(strnode), "malloc", "modules");
		return -1;
	}
	return 0;
}

/**
 * Delete a module from the module cache
 */
static void
module_cache_del(const char *name, const char *name_end)
{
	mh_int_t i = mh_strnptr_find_inp(modules, name, name_end - name);
	if (i == mh_end(modules))
		return;
	mh_strnptr_del(modules, i, NULL);
}

/*
 * Load a dso.
 * Create a new symlink based on temporary directory and try to
 * load via this symink to load a dso twice for cases of a function
 * reload.
 */
static struct module *
module_load(const char *package, const char *package_end)
{
	char path[PATH_MAX];
	if (module_find(package, package_end, path, sizeof(path)) != 0)
		return NULL;

	struct module *module = (struct module *) malloc(sizeof(*module));
	if (module == NULL) {
		diag_set(OutOfMemory, sizeof(struct module), "malloc",
			 "struct module");
		return NULL;
	}
	rlist_create(&module->funcs);
	module->calls = 0;
	module->is_unloading = false;
	char load_name[PATH_MAX + 1];
	if (tmpnam(load_name) == NULL) {
		diag_set(SystemError, "failed to create unique filename");
		goto error;
	}
	if (symlink(path, load_name) < 0) {
		diag_set(SystemError, "failed to create dso link");
		goto error;
	}
	module->handle = dlopen(load_name, RTLD_NOW | RTLD_LOCAL);
	if (unlink(load_name) != 0) {
		say_warn("failed to unlink dso link %s", load_name);
	}
	if (module->handle == NULL) {
		int package_len = (int) (package_end - package_end);
		diag_set(ClientError, ER_LOAD_MODULE, package_len,
			  package, dlerror());
		goto error;
	}

	return module;
error:
	free(module);
	return NULL;
}

static void
module_delete(struct module *module)
{
	dlclose(module->handle);
	TRASH(module);
	free(module);
}

/*
 * Check if a dso is unused and can be closed.
 */
static void
module_gc(struct module *module)
{
	if (!module->is_unloading || !rlist_empty(&module->funcs) ||
	     module->calls != 0)
		return;
	module_delete(module);
}

/*
 * Import a function from the module.
 */
static box_function_f
module_sym(struct module *module, const char *name)
{
	box_function_f f = (box_function_f)dlsym(module->handle, name);
	if (f == NULL) {
		diag_set(ClientError, ER_LOAD_FUNCTION, name, dlerror());
		return NULL;
	}
	return f;
}

/*
 * Reload a dso.
 */
struct module *
module_reload(const char *package, const char *package_end)
{
	struct module *old_module = module_cache_find(package, package_end);
	if (old_module == NULL) {
		/* Module wasn't loaded - do nothing. */
		old_module = module_load(package, package_end);
		if (old_module == NULL)
			return NULL;
		if (module_cache_put(package, package_end, old_module) != 0) {
			module_delete(old_module);
			return NULL;
		}
	}

	struct module *new_module = module_load(package, package_end);
	if (new_module == NULL)
		return NULL;

	struct func *func, *tmp_func;
	rlist_foreach_entry_safe(func, &old_module->funcs, item, tmp_func) {
		struct func_name name;
		func_split_name(func->def->name, &name);
		func->func = module_sym(new_module, name.sym);
		if (func->func == NULL)
			goto restore;
		func->module = new_module;
		rlist_move(&new_module->funcs, &func->item);
	}
	module_cache_del(package, package_end);
	if (module_cache_put(package, package_end, new_module) != 0)
		goto restore;
	old_module->is_unloading = true;
	module_gc(old_module);
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
	assert(rlist_empty(&new_module->funcs));
	module_delete(new_module);
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
		if (rlist_empty(&func->module->funcs)) {
			struct func_name name;
			func_split_name(func->def->name, &name);
			module_cache_del(name.package, name.package_end);
		}
		module_gc(func->module);
	}
	func->module = NULL;
	func->func = NULL;
}

/**
 * Resolve func->func (find the respective DLL and fetch the
 * symbol from it).
 */
static int
func_load(struct func *func)
{
	assert(func->func == NULL);

	struct func_name name;
	func_split_name(func->def->name, &name);

	struct module *module = module_cache_find(name.package,
						  name.package_end);
	if (module == NULL) {
		/* Try to find loaded module in the cache */
		module = module_load(name.package, name.package_end);
		if (module == NULL)
			diag_raise();
		if (module_cache_put(name.package, name.package_end, module)) {
			module_delete(module);
			diag_raise();
		}
	}

	func->func = module_sym(module, name.sym);
	if (func->func == NULL)
		return -1;
	func->module = module;
	rlist_add(&module->funcs, &func->item);
	return 0;
}

int
func_reload(struct func *func)
{
	struct func_name name;
	func_split_name(func->def->name, &name);
	struct module *module = module_reload(name.package, name.package_end);
	if (module == NULL) {
		diag_log();
		return -1;
	}
	return 0;
}

int
func_call(struct func *func, box_function_ctx_t *ctx, const char *args,
	  const char *args_end)
{
	if (func->func == NULL) {
		if (func_load(func) != 0)
			return -1;
	}

	/* Module can be changed after function reload. */
	struct module *module = func->module;
	if (module != NULL)
		++module->calls;
	int rc = func->func(ctx, args, args_end);
	if (module != NULL)
		--module->calls;
	module_gc(module);
	return rc;
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
