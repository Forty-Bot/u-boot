// SPDX-License-Identifier: GPL-2.0+ AND Zlib
/*
 * LIL - Little Interpreted Language
 * Copyright (C) 2021 Sean Anderson <seanga2@gmail.com>
 * Copyright (C) 2010-2013 Kostas Michalopoulos <badsector@runtimelegend.com>
 *
 * This file originated from the LIL project, licensed under Zlib
 * All changes licensed under GPL-2.0+
 */

#include <common.h>
#include <cli_lil.h>
#include <console.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Enable pools for reusing values, lists and environments. This will use more memory and
 * will rely on the runtime/OS to free the pools once the program ends, but will cause
 * considerably less memory fragmentation and improve the script execution performance. */
/*#define LIL_ENABLE_POOLS*/

/* Enable limiting recursive calls to lil_parse - this can be used to avoid call stack
 * overflows and is also useful when running through an automated fuzzer like AFL */
/*#define LIL_ENABLE_RECLIMIT 10000*/

#define MAX_CATCHER_DEPTH 16384
#define HASHMAP_CELLS 256
#define HASHMAP_CELLMASK 0xFF

struct hashentry {
	char *k;
	void *v;
};

struct hashcell {
	struct hashentry *e;
	size_t c;
};

struct hashmap {
	struct hashcell cell[HASHMAP_CELLS];
};

struct lil_value {
	size_t l;
#ifdef LIL_ENABLE_POOLS
	size_t c;
#endif
	char *d;
};

struct lil_var {
	char *n;
	char *w;
	struct lil_env *env;
	struct lil_value *v;
};

struct lil_env {
	struct lil_env *parent;
	struct lil_func *func;
	struct lil_value *catcher_for;
	struct lil_var **var;
	size_t vars;
	struct hashmap varmap;
	struct lil_value *retval;
	int retval_set;
	int breakrun;
};

struct lil_list {
	struct lil_value **v;
	size_t c;
	size_t cap;
};

struct lil_func {
	char *name;
	struct lil_value *code;
	struct lil_list *argnames;
	lil_func_proc_t proc;
};

struct lil {
	const char *code; /* need save on parse */
	const char *rootcode;
	size_t clen; /* need save on parse */
	size_t head; /* need save on parse */
	int ignoreeol;
	struct lil_func **cmd;
	size_t cmds;
	size_t syscmds;
	struct hashmap cmdmap;
	char *catcher;
	int in_catcher;
	char *dollarprefix;
	struct lil_env *env;
	struct lil_env *rootenv;
	struct lil_env *downenv;
	struct lil_value *empty;
	enum {
		ERROR_NOERROR = 0,
		ERROR_DEFAULT,
		ERROR_FIXHEAD,
		ERROR_UNBALANCED,
	} error;
	size_t err_head;
	char *err_msg;
	size_t parse_depth;
};

struct expreval {
	const char *code;
	size_t len, head;
	ssize_t ival;
	int error;
};

static struct lil_value *next_word(struct lil *lil);
static void register_stdcmds(struct lil *lil);
static void lil_set_error(struct lil *lil, const char *msg);
static void lil_set_errorf(struct lil *lil, const char *fmt, ...)
	__attribute((format(__printf__, 2, 3)));
static void lil_set_error_at(struct lil *lil, size_t pos, const char *msg);
static void
lil_set_errorf_at(struct lil *lil, size_t pos, const char *fmt, ...)
	__attribute((format(__printf__, 3, 4)));
static void lil_set_error_unbalanced(struct lil *lil, char expected);

#ifdef LIL_ENABLE_POOLS
static struct lil_value **pool;
static int poolsize, poolcap;
static struct lil_list **listpool;
static size_t listpoolsize, listpoolcap;
static struct lil_env **envpool;
static size_t envpoolsize, envpoolcap;
#endif

static unsigned long hm_hash(const char *key)
{
	unsigned long hash = 5381;
	int c;

	while ((c = *key++))
		hash = ((hash << 5) + hash) + c;
	return hash;
}

static void hm_init(struct hashmap *hm)
{
	memset(hm, 0, sizeof(struct hashmap));
}

static void hm_destroy(struct hashmap *hm)
{
	size_t i, j;

	for (i = 0; i < HASHMAP_CELLS; i++) {
		for (j = 0; j < hm->cell[i].c; j++)
			free(hm->cell[i].e[j].k);
		free(hm->cell[i].e);
	}
}

static void hm_put(struct hashmap *hm, const char *key, void *value)
{
	struct hashcell *cell = hm->cell + (hm_hash(key) & HASHMAP_CELLMASK);
	size_t i;

	for (i = 0; i < cell->c; i++) {
		if (!strcmp(key, cell->e[i].k)) {
			cell->e[i].v = value;
			return;
		}
	}

	cell->e = realloc(cell->e, sizeof(struct hashentry) * (cell->c + 1));
	cell->e[cell->c].k = strdup(key);
	cell->e[cell->c].v = value;
	cell->c++;
}

static void *hm_get(struct hashmap *hm, const char *key)
{
	struct hashcell *cell = hm->cell + (hm_hash(key) & HASHMAP_CELLMASK);
	size_t i;

	for (i = 0; i < cell->c; i++)
		if (!strcmp(key, cell->e[i].k))
			return cell->e[i].v;
	return NULL;
}

static int hm_has(struct hashmap *hm, const char *key)
{
	struct hashcell *cell = hm->cell + (hm_hash(key) & HASHMAP_CELLMASK);
	size_t i;

	for (i = 0; i < cell->c; i++)
		if (!strcmp(key, cell->e[i].k))
			return 1;
	return 0;
}

#ifdef LIL_ENABLE_POOLS
static struct lil_value *alloc_from_pool(void)
{
	if (poolsize > 0) {
		poolsize--;
		return pool[poolsize];
	} else {
		struct lil_value *val = calloc(1, sizeof(struct lil_value));

		return val;
	}
}

static void release_to_pool(struct lil_value *val)
{
	if (poolsize == poolcap) {
		poolcap = poolcap ? (poolcap + poolcap / 2) : 64;
		pool = realloc(pool, sizeof(struct lil_value *) * poolcap);
	}
	pool[poolsize++] = val;
}

static void ensure_capacity(struct lil_value *val, size_t cap)
{
	if (val->c < cap) {
		val->c = cap + 128;
		val->d = realloc(val->d, val->c);
	}
}
#endif

static struct lil_value *alloc_value_len(const char *str, size_t len)
{
#ifdef LIL_ENABLE_POOLS
	struct lil_value *val = alloc_from_pool();
#else
	struct lil_value *val = calloc(1, sizeof(struct lil_value));
#endif

	if (!val)
		return NULL;
	if (str) {
		val->l = len;
#ifdef LIL_ENABLE_POOLS
		ensure_capacity(val, len + 1);
#else
		val->d = malloc(len + 1);
		if (!val->d) {
			free(val);
			return NULL;
		}
#endif
		memcpy(val->d, str, len);
		val->d[len] = 0;
	} else {
		val->l = 0;
#ifdef LIL_ENABLE_POOLS
		ensure_capacity(val, 1);
		val->d[0] = '\0';
#else
		val->d = NULL;
#endif
	}
	return val;
}

static struct lil_value *alloc_value(const char *str)
{
	return alloc_value_len(str, str ? strlen(str) : 0);
}

struct lil_value *lil_clone_value(struct lil_value *src)
{
	struct lil_value *val;

	if (!src)
		return NULL;
#ifdef LIL_ENABLE_POOLS
	val = alloc_from_pool();
#else
	val = calloc(1, sizeof(struct lil_value));
#endif
	if (!val)
		return NULL;

	val->l = src->l;
	if (src->l) {
#ifdef LIL_ENABLE_POOLS
		ensure_capacity(val, val->l + 1);
#else
		val->d = malloc(val->l + 1);
		if (!val->d) {
			free(val);
			return NULL;
		}
#endif
		memcpy(val->d, src->d, val->l + 1);
	} else {
#ifdef LIL_ENABLE_POOLS
		ensure_capacity(val, 1);
		val->d[0] = '\0';
#else
		val->d = NULL;
#endif
	}
	return val;
}

int lil_append_char(struct lil_value *val, char ch)
{
#ifdef LIL_ENABLE_POOLS
	ensure_capacity(val, val->l + 2);
	val->d[val->l++] = ch;
	val->d[val->l] = '\0';
#else
	char *new = realloc(val->d, val->l + 2);

	if (!new)
		return 0;

	new[val->l++] = ch;
	new[val->l] = 0;
	val->d = new;
#endif
	return 1;
}

int lil_append_string_len(struct lil_value *val, const char *s, size_t len)
{
#ifndef LIL_ENABLE_POOLS
	char *new;
#endif

	if (!s || !s[0])
		return 1;

#ifdef LIL_ENABLE_POOLS
	ensure_capacity(val, val->l + len + 1);
	memcpy(val->d + val->l, s, len + 1);
#else
	new = realloc(val->d, val->l + len + 1);
	if (!new)
		return 0;

	memcpy(new + val->l, s, len + 1);
	val->d = new;
#endif
	val->l += len;
	return 1;
}

int lil_append_string(struct lil_value *val, const char *s)
{
	return lil_append_string_len(val, s, strlen(s));
}

int lil_append_val(struct lil_value *val, struct lil_value *v)
{
#ifndef LIL_ENABLE_POOLS
	char *new;
#endif

	if (!v || !v->l)
		return 1;

#ifdef LIL_ENABLE_POOLS
	ensure_capacity(val, val->l + v->l + 1);
	memcpy(val->d + val->l, v->d, v->l + 1);
#else
	new = realloc(val->d, val->l + v->l + 1);
	if (!new)
		return 0;

	memcpy(new + val->l, v->d, v->l + 1);
	val->d = new;
#endif
	val->l += v->l;
	return 1;
}

void lil_free_value(struct lil_value *val)
{
	if (!val)
		return;

#ifdef LIL_ENABLE_POOLS
	release_to_pool(val);
#else
	free(val->d);
	free(val);
#endif
}

struct lil_list *lil_alloc_list(void)
{
	struct lil_list *list;

#ifdef LIL_ENABLE_POOLS
	if (listpoolsize > 0)
		return listpool[--listpoolsize];
#endif
	list = calloc(1, sizeof(struct lil_list));
	list->v = NULL;
	return list;
}

void lil_free_list(struct lil_list *list)
{
	size_t i;

	if (!list)
		return;

	for (i = 0; i < list->c; i++)
		lil_free_value(list->v[i]);

#ifdef LIL_ENABLE_POOLS
	list->c = 0;
	if (listpoolsize == listpoolcap) {
		listpoolcap =
			listpoolcap ? (listpoolcap + listpoolcap / 2) : 32;
		listpool = realloc(listpool,
				   sizeof(struct lil_list *) * listpoolcap);
	}
	listpool[listpoolsize++] = list;
#else
	free(list->v);
	free(list);
#endif
}

void lil_list_append(struct lil_list *list, struct lil_value *val)
{
	if (list->c == list->cap) {
		size_t cap = list->cap ? (list->cap + list->cap / 2) : 32;
		struct lil_value **nv =
			realloc(list->v, sizeof(struct lil_value *) * cap);

		if (!nv)
			return;

		list->cap = cap;
		list->v = nv;
	}
	list->v[list->c++] = val;
}

size_t lil_list_size(struct lil_list *list)
{
	return list->c;
}

struct lil_value *lil_list_get(struct lil_list *list, size_t index)
{
	return index >= list->c ? NULL : list->v[index];
}

static int needs_escape(const char *str)
{
	size_t i;

	if (!str || !str[0])
		return 1;

	for (i = 0; str[i]; i++)
		if (ispunct(str[i]) || isspace(str[i]))
			return 1;

	return 0;
}

struct lil_value *lil_list_to_value(struct lil_list *list, int do_escape)
{
	struct lil_value *val = alloc_value(NULL);
	size_t i, j;

	for (i = 0; i < list->c; i++) {
		int escape =
			do_escape ? needs_escape(lil_to_string(list->v[i])) : 0;

		if (i)
			lil_append_char(val, ' ');

		if (escape) {
			lil_append_char(val, '{');
			for (j = 0; j < list->v[i]->l; j++) {
				if (list->v[i]->d[j] == '{')
					lil_append_string(val, "}\"\\o\"{");
				else if (list->v[i]->d[j] == '}')
					lil_append_string(val, "}\"\\c\"{");
				else
					lil_append_char(val, list->v[i]->d[j]);
			}
			lil_append_char(val, '}');
		} else {
			lil_append_val(val, list->v[i]);
		}
	}
	return val;
}

struct lil_env *lil_alloc_env(struct lil_env *parent)
{
	struct lil_env *env;

#ifdef LIL_ENABLE_POOLS
	if (envpoolsize > 0) {
		size_t i, j;

		env = envpool[--envpoolsize];
		env->parent = parent;
		env->func = NULL;
		env->catcher_for = NULL;
		env->var = NULL;
		env->vars = 0;
		env->retval = NULL;
		env->retval_set = 0;
		env->breakrun = 0;
		for (i = 0; i < HASHMAP_CELLS; i++) {
			for (j = 0; j < env->varmap.cell[i].c; j++)
				free(env->varmap.cell[i].e[j].k);
			env->varmap.cell[i].c = 0;
		}
		return env;
	}
#endif
	env = calloc(1, sizeof(struct lil_env));
	env->parent = parent;
	return env;
}

void lil_free_env(struct lil_env *env)
{
	size_t i;

	if (!env)
		return;

	lil_free_value(env->retval);
#ifdef LIL_ENABLE_POOLS
	for (i = 0; i < env->vars; i++) {
		free(env->var[i]->n);
		lil_free_value(env->var[i]->v);
		free(env->var[i]->w);
		free(env->var[i]);
	}
	free(env->var);

	if (envpoolsize == envpoolcap) {
		envpoolcap = envpoolcap ? (envpoolcap + envpoolcap / 2) : 64;
		envpool =
			realloc(envpool, sizeof(struct lil_env *) * envpoolcap);
	}
	envpool[envpoolsize++] = env;
#else
	hm_destroy(&env->varmap);
	for (i = 0; i < env->vars; i++) {
		free(env->var[i]->n);
		lil_free_value(env->var[i]->v);
		free(env->var[i]->w);
		free(env->var[i]);
	}
	free(env->var);
	free(env);
#endif
}

static struct lil_var *lil_find_local_var(struct lil *lil, struct lil_env *env,
					  const char *name)
{
	return hm_get(&env->varmap, name);
}

static struct lil_var *lil_find_var(struct lil *lil, struct lil_env *env,
				    const char *name)
{
	struct lil_var *r = lil_find_local_var(lil, env, name);

	if (r)
		return r;

	if (env == lil->rootenv)
		return NULL;

	return lil_find_var(lil, lil->rootenv, name);
}

static struct lil_func *lil_find_cmd(struct lil *lil, const char *name)
{
	struct lil_func *r;
	char *dot = strchr(name, '.');

	/* Some U-Boot commands have dots in their names */
	if (dot)
		*dot = '\0';
	r = hm_get(&lil->cmdmap, name);

	if (dot)
		*dot = '.';
	return r;
}

static struct lil_func *add_func(struct lil *lil, const char *name)
{
	struct lil_func *cmd;
	struct lil_func **ncmd;

	cmd = lil_find_cmd(lil, name);
	if (cmd) {
		if (cmd->argnames)
			lil_free_list(cmd->argnames);
		lil_free_value(cmd->code);
		cmd->argnames = NULL;
		cmd->code = NULL;
		cmd->proc = NULL;
		return cmd;
	}

	cmd = calloc(1, sizeof(struct lil_func));
	cmd->name = strdup(name);

	ncmd = realloc(lil->cmd, sizeof(struct lil_func *) * (lil->cmds + 1));
	if (!ncmd) {
		free(cmd);
		return NULL;
	}

	lil->cmd = ncmd;
	ncmd[lil->cmds++] = cmd;
	hm_put(&lil->cmdmap, name, cmd);
	return cmd;
}

static void del_func(struct lil *lil, struct lil_func *cmd)
{
	size_t i, index = lil->cmds;

	for (i = 0; i < lil->cmds; i++) {
		if (lil->cmd[i] == cmd) {
			index = i;
			break;
		}
	}
	if (index == lil->cmds)
		return;

	hm_put(&lil->cmdmap, cmd->name, 0);
	if (cmd->argnames)
		lil_free_list(cmd->argnames);

	lil_free_value(cmd->code);
	free(cmd->name);
	free(cmd);
	lil->cmds--;
	for (i = index; i < lil->cmds; i++)
		lil->cmd[i] = lil->cmd[i + 1];
}

int lil_register(struct lil *lil, const char *name, lil_func_proc_t proc)
{
	struct lil_func *cmd = add_func(lil, name);

	if (!cmd)
		return 0;
	cmd->proc = proc;
	return 1;
}

struct lil_var *lil_set_var(struct lil *lil, const char *name,
			    struct lil_value *val, enum lil_setvar local)
{
	struct lil_var **nvar;
	struct lil_env *env =
		local == LIL_SETVAR_GLOBAL ? lil->rootenv : lil->env;
	int freeval = 0;

	if (!name[0])
		return NULL;

	if (local != LIL_SETVAR_LOCAL_NEW) {
		struct lil_var *var = lil_find_var(lil, env, name);

		if (local == LIL_SETVAR_LOCAL_ONLY && var &&
		    var->env == lil->rootenv && var->env != env)
			var = NULL;

		if ((!var && env == lil->rootenv) ||
		     (var && var->env == lil->rootenv)) {
			if (env_set(name, val->d))
				return NULL;
		}

		if (var) {
			lil_free_value(var->v);
			var->v = freeval ? val : lil_clone_value(val);
			if (var->w) {
				struct lil_env *save_env;

				save_env = lil->env;
				lil->env = var->env;
				lil_free_value(lil_parse(lil, var->w, 0, 1));
				lil->env = save_env;
			}
			return var;
		}
	}

	nvar = realloc(env->var, sizeof(struct lil_var *) * (env->vars + 1));
	if (!nvar) {
		/* TODO: report memory error */
		return NULL;
	}

	env->var = nvar;
	nvar[env->vars] = calloc(1, sizeof(struct lil_var));
	nvar[env->vars]->n = strdup(name);
	nvar[env->vars]->w = NULL;
	nvar[env->vars]->env = env;
	nvar[env->vars]->v = freeval ? val : lil_clone_value(val);
	hm_put(&env->varmap, name, nvar[env->vars]);
	return nvar[env->vars++];
}

struct lil_value *lil_get_var(struct lil *lil, const char *name)
{
	return lil_get_var_or(lil, name, lil->empty);
}

struct lil_value *lil_get_var_or(struct lil *lil, const char *name,
				 struct lil_value *defvalue)
{
	struct lil_var *var = lil_find_var(lil, lil->env, name);
	struct lil_value *retval = var ? var->v : defvalue;

	if (!var || var->env == lil->rootenv) {
		struct lil_value *newretval = lil_alloc_string(env_get(name));

		if (newretval)
			retval = newretval;
	}
	return retval;
}

struct lil_env *lil_push_env(struct lil *lil)
{
	struct lil_env *env = lil_alloc_env(lil->env);

	lil->env = env;
	return env;
}

void lil_pop_env(struct lil *lil)
{
	if (lil->env->parent) {
		struct lil_env *next = lil->env->parent;

		lil_free_env(lil->env);
		lil->env = next;
	}
}

struct lil *lil_new(void)
{
	struct lil *lil = calloc(1, sizeof(struct lil));

	lil->rootenv = lil->env = lil_alloc_env(NULL);
	lil->empty = alloc_value(NULL);
	lil->dollarprefix = strdup("set ");
	hm_init(&lil->cmdmap);
	register_stdcmds(lil);
	return lil;
}

static int islilspecial(char ch)
{
	return ch == '$' || ch == '{' || ch == '}' || ch == '[' || ch == ']' ||
	       ch == '"' || ch == '\'' || ch == ';';
}

static int eolchar(char ch)
{
	return ch == '\n' || ch == '\r' || ch == ';';
}

static int ateol(struct lil *lil)
{
	return !(lil->ignoreeol) && eolchar(lil->code[lil->head]);
}

static void lil_skip_spaces(struct lil *lil)
{
	while (lil->head < lil->clen) {
		if (lil->code[lil->head] == '#') {
			if (lil->code[lil->head + 1] == '#' &&
			    lil->code[lil->head + 2] != '#') {
				lil->head += 2;
				while (lil->head < lil->clen) {
					if ((lil->code[lil->head] == '#') &&
					    (lil->code[lil->head + 1] == '#') &&
					    (lil->code[lil->head + 2] != '#')) {
						lil->head += 2;
						break;
					}
					lil->head++;
				}
			} else {
				while (lil->head < lil->clen &&
				       !eolchar(lil->code[lil->head]))
					lil->head++;
			}
		} else if (lil->code[lil->head] == '\\' &&
			   eolchar(lil->code[lil->head + 1])) {
			lil->head++;
			while (lil->head < lil->clen &&
			       eolchar(lil->code[lil->head]))
				lil->head++;
		} else if (eolchar(lil->code[lil->head])) {
			if (lil->ignoreeol)
				lil->head++;
			else
				break;
		} else if (isspace(lil->code[lil->head]))
			lil->head++;
		else
			break;
	}
}

static struct lil_value *get_bracketpart(struct lil *lil)
{
	size_t cnt = 1;
	struct lil_value *val = NULL;
	struct lil_value *cmd = alloc_value(NULL);
	int save_eol = lil->ignoreeol;

	lil->ignoreeol = 0;
	lil->head++;
	while (lil->head < lil->clen) {
		if (lil->code[lil->head] == '[') {
			lil->head++;
			cnt++;
			lil_append_char(cmd, '[');
		} else if (lil->code[lil->head] == ']') {
			lil->head++;
			if (--cnt == 0)
				break;
			else
				lil_append_char(cmd, ']');
		} else {
			lil_append_char(cmd, lil->code[lil->head++]);
		}
	}

	if (cnt)
		lil_set_error_unbalanced(lil, ']');
	else
		val = lil_parse_value(lil, cmd, 0);
	lil_free_value(cmd);
	lil->ignoreeol = save_eol;
	return val;
}

static struct lil_value *get_dollarpart(struct lil *lil)
{
	struct lil_value *val, *name, *tmp;

	lil->head++;
	name = next_word(lil);
	tmp = alloc_value(lil->dollarprefix);
	lil_append_val(tmp, name);
	lil_free_value(name);

	val = lil_parse_value(lil, tmp, 0);
	lil_free_value(tmp);
	return val;
}

static struct lil_value *next_word(struct lil *lil)
{
	struct lil_value *val;
	size_t start;

	lil_skip_spaces(lil);
	if (lil->code[lil->head] == '$') {
		val = get_dollarpart(lil);
	} else if (lil->code[lil->head] == '{') {
		size_t cnt = 1;

		lil->head++;
		val = alloc_value(NULL);
		while (lil->head < lil->clen) {
			if (lil->code[lil->head] == '{') {
				lil->head++;
				cnt++;
				lil_append_char(val, '{');
			} else if (lil->code[lil->head] == '}') {
				lil->head++;
				if (--cnt == 0)
					break;
				else
					lil_append_char(val, '}');
			} else {
				lil_append_char(val, lil->code[lil->head++]);
			}
		}

		if (cnt) {
			lil_set_error_unbalanced(lil, '}');
			lil_free_value(val);
			val = NULL;
		}
	} else if (lil->code[lil->head] == '[') {
		val = get_bracketpart(lil);
	} else if (lil->code[lil->head] == '"' ||
		   lil->code[lil->head] == '\'') {
		bool matched = false;
		char sc = lil->code[lil->head++];

		val = alloc_value(NULL);
		while (lil->head < lil->clen) {
			if (lil->code[lil->head] == '[' ||
			    lil->code[lil->head] == '$') {
				struct lil_value *tmp =
					lil->code[lil->head] == '$' ?
						      get_dollarpart(lil) :
						      get_bracketpart(lil);

				lil_append_val(val, tmp);
				lil_free_value(tmp);
				lil->head--; /* avoid skipping the char below */
			} else if (lil->code[lil->head] == '\\') {
				lil->head++;
				switch (lil->code[lil->head]) {
				case 'b':
					lil_append_char(val, '\b');
					break;
				case 't':
					lil_append_char(val, '\t');
					break;
				case 'n':
					lil_append_char(val, '\n');
					break;
				case 'v':
					lil_append_char(val, '\v');
					break;
				case 'f':
					lil_append_char(val, '\f');
					break;
				case 'r':
					lil_append_char(val, '\r');
					break;
				case '0':
					lil_append_char(val, 0);
					break;
				case 'a':
					lil_append_char(val, '\a');
					break;
				case 'c':
					lil_append_char(val, '}');
					break;
				case 'o':
					lil_append_char(val, '{');
					break;
				default:
					lil_append_char(val,
							lil->code[lil->head]);
					break;
				}
			} else if (lil->code[lil->head] == sc) {
				matched = true;
				lil->head++;
				break;
			} else {
				lil_append_char(val, lil->code[lil->head]);
			}
			lil->head++;
		}

		if (!matched) {
			lil_set_error_unbalanced(lil, sc);
			lil_free_value(val);
			val = NULL;
		}
	} else {
		start = lil->head;
		while (lil->head < lil->clen &&
		       !isspace(lil->code[lil->head]) &&
		       !islilspecial(lil->code[lil->head]))
			lil->head++;
		val = alloc_value_len(lil->code + start, lil->head - start);
	}
	return val ? val : alloc_value(NULL);
}

static struct lil_list *substitute(struct lil *lil)
{
	struct lil_list *words = lil_alloc_list();

	lil_skip_spaces(lil);
	while (lil->head < lil->clen && !ateol(lil) && !lil->error) {
		struct lil_value *w = alloc_value(NULL);

		do {
			size_t head = lil->head;
			struct lil_value *wp = next_word(lil);

			if (head ==
			    lil->head) { /* something wrong, the parser can't proceed */
				lil_free_value(w);
				lil_free_value(wp);
				lil_free_list(words);
				return NULL;
			}

			lil_append_val(w, wp);
			lil_free_value(wp);
		} while (lil->head < lil->clen &&
			 !eolchar(lil->code[lil->head]) &&
			 !isspace(lil->code[lil->head]) && !lil->error);
		lil_skip_spaces(lil);

		lil_list_append(words, w);
	}

	return words;
}

struct lil_list *lil_subst_to_list(struct lil *lil, struct lil_value *code)
{
	const char *save_code = lil->code;
	size_t save_clen = lil->clen;
	size_t save_head = lil->head;
	int save_igeol = lil->ignoreeol;
	struct lil_list *words;

	lil->code = lil_to_string(code);
	lil->clen = code->l;
	lil->head = 0;
	lil->ignoreeol = 1;

	words = substitute(lil);
	if (!words)
		words = lil_alloc_list();

	lil->code = save_code;
	lil->clen = save_clen;
	lil->head = save_head;
	lil->ignoreeol = save_igeol;
	return words;
}

struct lil_value *lil_subst_to_value(struct lil *lil, struct lil_value *code)
{
	struct lil_list *words = lil_subst_to_list(lil, code);
	struct lil_value *val;

	val = lil_list_to_value(words, 0);
	lil_free_list(words);
	return val;
}

static struct lil_value *unknown_cmd(struct lil *lil, struct lil_list *words)
{
	struct lil_value *r = NULL;

	if (IS_ENABLED(CONFIG_LIL_FULL) && lil->catcher) {
		if (lil->in_catcher < MAX_CATCHER_DEPTH) {
			struct lil_value *args;

			lil->in_catcher++;
			lil_push_env(lil);
			lil->env->catcher_for = words->v[0];

			args = lil_list_to_value(words, 1);
			lil_set_var(lil, "args", args, LIL_SETVAR_LOCAL_NEW);
			lil_free_value(args);
			r = lil_parse(lil, lil->catcher, 0, 1);

			lil_pop_env(lil);
			lil->in_catcher--;
		} else {
			lil_set_errorf_at(lil, lil->head,
					  "catcher limit reached while trying to call unknown function %s",
					  words->v[0]->d);
		}
	} else {
		lil_set_errorf_at(lil, lil->head, "unknown function %s",
				  words->v[0]->d);
	}

	return r;
}

static struct lil_value *run_cmd(struct lil *lil, struct lil_func *cmd,
				 struct lil_list *words)
{
	struct lil_value *r;

	if (cmd->proc) {
		size_t shead = lil->head;
		r = cmd->proc(lil, words->c - 1, words->v + 1);

		if (lil->error == ERROR_FIXHEAD) {
			lil->error = ERROR_DEFAULT;
			lil->err_head = shead;
		}
	} else {
		lil_push_env(lil);
		lil->env->func = cmd;

		if (cmd->argnames->c == 1 &&
		    !strcmp(lil_to_string(cmd->argnames->v[0]), "args")) {
			struct lil_value *args = lil_list_to_value(words, 1);

			lil_set_var(lil, "args", args, LIL_SETVAR_LOCAL_NEW);
			lil_free_value(args);
		} else {
			size_t i;

			for (i = 0; i < cmd->argnames->c; i++) {
				struct lil_value *val;

				if (i < words->c - 1)
					val = words->v[i + 1];
				else
					val = lil->empty;

				lil_set_var(lil,
					    lil_to_string(cmd->argnames->v[i]),
					    val, LIL_SETVAR_LOCAL_NEW);
			}
		}
		r = lil_parse_value(lil, cmd->code, 1);

		lil_pop_env(lil);
	}

	return r;
}

struct lil_value *lil_parse(struct lil *lil, const char *code, size_t codelen,
			    int funclevel)
{
	const char *save_code = lil->code;
	size_t save_clen = lil->clen;
	size_t save_head = lil->head;
	struct lil_value *val = NULL;
	struct lil_list *words = NULL;

	if (!save_code)
		lil->rootcode = code;
	lil->code = code;
	lil->clen = codelen ? codelen : strlen(code);
	lil->head = 0;

	lil_skip_spaces(lil);
	lil->parse_depth++;
#ifdef LIL_ENABLE_RECLIMIT
	if (lil->parse_depth > LIL_ENABLE_RECLIMIT) {
		lil_set_error(lil, "Too many recursive calls");
		goto cleanup;
	}
#endif

	if (lil->parse_depth == 1)
		lil->error = 0;

	if (funclevel)
		lil->env->breakrun = 0;

	while (lil->head < lil->clen && !lil->error) {
		if (words)
			lil_free_list(words);

		if (val)
			lil_free_value(val);
		val = NULL;

		if (ctrlc()) {
			lil_set_error_at(lil, lil->head, "interrupted");
			goto cleanup;
		}

		words = substitute(lil);
		if (!words || lil->error)
			goto cleanup;

		if (words->c) {
			struct lil_func *cmd =
				lil_find_cmd(lil, lil_to_string(words->v[0]));

			if (!cmd) {
				if (words->v[0]->l) {
					val = unknown_cmd(lil, words);
					if (!val)
						goto cleanup;
				}
			} else {
				val = run_cmd(lil, cmd, words);
			}

			if (lil->env->breakrun)
				goto cleanup;
		}

		lil_skip_spaces(lil);
		while (ateol(lil))
			lil->head++;
		lil_skip_spaces(lil);
	}

cleanup:
	if (words)
		lil_free_list(words);
	lil->code = save_code;
	lil->clen = save_clen;
	lil->head = save_head;

	if (funclevel && lil->env->retval_set) {
		if (val)
			lil_free_value(val);
		val = lil->env->retval;
		lil->env->retval = NULL;
		lil->env->retval_set = 0;
		lil->env->breakrun = 0;
	}

	lil->parse_depth--;
	return val ? val : alloc_value(NULL);
}

struct lil_value *lil_parse_value(struct lil *lil, struct lil_value *val,
				  int funclevel)
{
	if (!val || !val->d || !val->l)
		return alloc_value(NULL);

	return lil_parse(lil, val->d, val->l, funclevel);
}

struct lil_value *lil_call(struct lil *lil, const char *funcname, size_t argc,
			   struct lil_value **argv)
{
	struct lil_func *cmd = lil_find_cmd(lil, funcname);
	struct lil_value *r = NULL;

	if (cmd) {
		if (cmd->proc) {
			r = cmd->proc(lil, argc, argv);
		} else {
			size_t i;

			lil_push_env(lil);
			lil->env->func = cmd;

			if (cmd->argnames->c == 1 &&
			    !strcmp(lil_to_string(cmd->argnames->v[0]),
				    "args")) {
				struct lil_list *args = lil_alloc_list();
				struct lil_value *argsval;

				for (i = 0; i < argc; i++)
					lil_list_append(
						args, lil_clone_value(argv[i]));
				argsval = lil_list_to_value(args, 0);
				lil_set_var(lil, "args", argsval,
					    LIL_SETVAR_LOCAL_NEW);

				lil_free_value(argsval);
				lil_free_list(args);
			} else {
				for (i = 0; i < cmd->argnames->c; i++) {
					struct lil_value *val = NULL;

					if (i < argc)
						val = argv[i];

					lil_set_var(
						lil,
						lil_to_string(
							cmd->argnames->v[i]),
						val, LIL_SETVAR_LOCAL_NEW);
				}
			}

			r = lil_parse_value(lil, cmd->code, 1);
			lil_pop_env(lil);
		}
	}

	return r;
}

static void lil_set_error(struct lil *lil, const char *msg)
{
	if (lil->error)
		return;

	free(lil->err_msg);
	lil->error = ERROR_FIXHEAD;
	lil->err_head = 0;
	lil->err_msg = strdup(msg ? msg : "");
}

static void lil_set_errorf(struct lil *lil, const char *fmt, ...)
{
	va_list args;
	char msg[CONFIG_SYS_PBSIZE];

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	return lil_set_error(lil, msg);
}

static void lil_set_error_at(struct lil *lil, size_t pos, const char *msg)
{
	if (lil->error)
		return;

	free(lil->err_msg);
	lil->error = ERROR_DEFAULT;
	lil->err_head = pos;
	lil->err_msg = strdup(msg ? msg : "");
}

static void lil_set_errorf_at(struct lil *lil, size_t pos, const char *fmt, ...)
{
	va_list args;
	char msg[CONFIG_SYS_PBSIZE];

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	return lil_set_error_at(lil, pos, msg);
}

void lil_set_error_unbalanced(struct lil *lil, char expected)
{
	if (lil->error)
		return;

	lil_set_errorf_at(lil, lil->head, "expected %c", expected);
	lil->error = ERROR_UNBALANCED;
}

int lil_error(struct lil *lil, const char **msg, size_t *pos)
{
	if (!lil->error)
		return 0;

	*msg = lil->err_msg;
	*pos = lil->err_head;
	lil->error = ERROR_NOERROR;

	return 1;
}

#define EERR_NO_ERROR 0
#define EERR_SYNTAX_ERROR 1
#define EERR_DIVISION_BY_ZERO 3
#define EERR_INVALID_EXPRESSION 4

static void ee_expr(struct expreval *ee);

static int ee_invalidpunct(int ch)
{
	return ispunct(ch) && ch != '!' && ch != '~' && ch != '(' &&
	       ch != ')' && ch != '-' && ch != '+';
}

static void ee_skip_spaces(struct expreval *ee)
{
	while (ee->head < ee->len && isspace(ee->code[ee->head]))
		ee->head++;
}

static void ee_numeric_element(struct expreval *ee)
{
	ee_skip_spaces(ee);
	ee->ival = 0;
	while (ee->head < ee->len) {
		if (!isdigit(ee->code[ee->head]))
			break;

		ee->ival = ee->ival * 10 + (ee->code[ee->head] - '0');
		ee->head++;
	}
}

static void ee_element(struct expreval *ee)
{
	if (isdigit(ee->code[ee->head])) {
		ee_numeric_element(ee);
		return;
	}

	/*
	 * for anything else that might creep in (usually from strings), we set
	 * the value to 1 so that strings evaluate as "true" when used in
	 * conditional expressions
	 */
	ee->ival = 1;
	ee->error = EERR_INVALID_EXPRESSION; /* special flag, will be cleared */
}

static void ee_paren(struct expreval *ee)
{
	ee_skip_spaces(ee);
	if (ee->code[ee->head] == '(') {
		ee->head++;
		ee_expr(ee);
		ee_skip_spaces(ee);

		if (ee->code[ee->head] == ')')
			ee->head++;
		else
			ee->error = EERR_SYNTAX_ERROR;
	} else {
		ee_element(ee);
	}
}

static void ee_unary(struct expreval *ee)
{
	ee_skip_spaces(ee);
	if (ee->head < ee->len && !ee->error &&
	    (ee->code[ee->head] == '-' || ee->code[ee->head] == '+' ||
	     ee->code[ee->head] == '~' || ee->code[ee->head] == '!')) {
		char op = ee->code[ee->head++];

		ee_unary(ee);
		if (ee->error)
			return;

		switch (op) {
		case '-':
			ee->ival = -ee->ival;
			break;
		case '+':
			/* ignore it, doesn't change a thing */
			break;
		case '~':
			ee->ival = ~ee->ival;
			break;
		case '!':
			ee->ival = !ee->ival;
			break;
		}
	} else {
		ee_paren(ee);
	}
}

static void ee_muldiv(struct expreval *ee)
{
	ee_unary(ee);
	if (ee->error || !IS_ENABLED(CONFIG_LIL_FULL))
		return;

	ee_skip_spaces(ee);
	while (ee->head < ee->len && !ee->error &&
	       !ee_invalidpunct(ee->code[ee->head + 1]) &&
	       (ee->code[ee->head] == '*' || ee->code[ee->head] == '/' ||
		ee->code[ee->head] == '\\' || ee->code[ee->head] == '%')) {
		ssize_t oival = ee->ival;

		switch (ee->code[ee->head]) {
		case '*':
			ee->head++;
			ee_unary(ee);
			if (ee->error)
				return;

			ee->ival = ee->ival * oival;
			break;
		case '%':
			ee->head++;
			ee_unary(ee);
			if (ee->error)
				return;

			if (ee->ival == 0) {
				ee->error = EERR_DIVISION_BY_ZERO;
			} else {
				ee->ival = oival % ee->ival;
			}
			break;
		case '/':
			ee->head++;
			ee_unary(ee);
			if (ee->error)
				return;

			if (ee->ival == 0) {
				ee->error = EERR_DIVISION_BY_ZERO;
			} else {
				ee->ival = oival / ee->ival;
			}
			break;
		case '\\':
			ee->head++;
			ee_unary(ee);
			if (ee->error)
				return;

			if (ee->ival == 0) {
				ee->error = EERR_DIVISION_BY_ZERO;
			} else {
				ee->ival = oival / ee->ival;
			}
			break;
		}

		ee_skip_spaces(ee);
	}
}

static void ee_addsub(struct expreval *ee)
{
	ee_muldiv(ee);
	if (!IS_ENABLED(CONFIG_LIL_FULL))
		return;

	ee_skip_spaces(ee);
	while (ee->head < ee->len && !ee->error &&
	       !ee_invalidpunct(ee->code[ee->head + 1]) &&
	       (ee->code[ee->head] == '+' || ee->code[ee->head] == '-')) {
		ssize_t oival = ee->ival;

		switch (ee->code[ee->head]) {
		case '+':
			ee->head++;
			ee_muldiv(ee);
			if (ee->error)
				return;

			ee->ival = ee->ival + oival;
			break;
		case '-':
			ee->head++;
			ee_muldiv(ee);
			if (ee->error)
				return;

			ee->ival = oival - ee->ival;
			break;
		}

		ee_skip_spaces(ee);
	}
}

static void ee_shift(struct expreval *ee)
{
	ee_addsub(ee);
	if (!IS_ENABLED(CONFIG_LIL_FULL))
		return;

	ee_skip_spaces(ee);
	while (ee->head < ee->len && !ee->error &&
	       ((ee->code[ee->head] == '<' && ee->code[ee->head + 1] == '<') ||
		(ee->code[ee->head] == '>' && ee->code[ee->head + 1] == '>'))) {
		ssize_t oival = ee->ival;

		ee->head++;
		switch (ee->code[ee->head]) {
		case '<':
			ee->head++;
			ee_addsub(ee);
			if (ee->error)
				return;

			ee->ival = oival << ee->ival;
			break;
		case '>':
			ee->head++;
			ee_addsub(ee);
			if (ee->error)
				return;

			ee->ival = oival >> ee->ival;
			break;
		}

		ee_skip_spaces(ee);
	}
}

static void ee_compare(struct expreval *ee)
{
	ee_shift(ee);
	ee_skip_spaces(ee);

	while (ee->head < ee->len && !ee->error &&
	       ((ee->code[ee->head] == '<' &&
		 !ee_invalidpunct(ee->code[ee->head + 1])) ||
		(ee->code[ee->head] == '>' &&
		 !ee_invalidpunct(ee->code[ee->head + 1])) ||
		(ee->code[ee->head] == '<' && ee->code[ee->head + 1] == '=') ||
		(ee->code[ee->head] == '>' && ee->code[ee->head + 1] == '='))) {
		ssize_t oival = ee->ival;
		int op = 4;

		if (ee->code[ee->head] == '<' &&
		    !ee_invalidpunct(ee->code[ee->head + 1]))
			op = 1;
		else if (ee->code[ee->head] == '>' &&
			 !ee_invalidpunct(ee->code[ee->head + 1]))
			op = 2;
		else if (ee->code[ee->head] == '<' &&
			 ee->code[ee->head + 1] == '=')
			op = 3;

		ee->head += op > 2 ? 2 : 1;

		switch (op) {
		case 1:
			ee_shift(ee);
			if (ee->error)
				return;

			ee->ival = (oival < ee->ival) ? 1 : 0;
			break;
		case 2:
			ee_shift(ee);
			if (ee->error)
				return;

			ee->ival = (oival > ee->ival) ? 1 : 0;
			break;
		case 3:
			ee_shift(ee);
			if (ee->error)
				return;

			ee->ival = (oival <= ee->ival) ? 1 : 0;
			break;
		case 4:
			ee_shift(ee);
			if (ee->error)
				return;

			ee->ival = (oival >= ee->ival) ? 1 : 0;
			break;
		}

		ee_skip_spaces(ee);
	}
}

static void ee_equals(struct expreval *ee)
{
	ee_compare(ee);
	ee_skip_spaces(ee);

	while (ee->head < ee->len && !ee->error &&
	       ((ee->code[ee->head] == '=' && ee->code[ee->head + 1] == '=') ||
		(ee->code[ee->head] == '!' && ee->code[ee->head + 1] == '='))) {
		ssize_t oival = ee->ival;
		int op = ee->code[ee->head] == '=' ? 1 : 2;

		ee->head += 2;

		switch (op) {
		case 1:
			ee_compare(ee);
			if (ee->error)
				return;

			ee->ival = (oival == ee->ival) ? 1 : 0;
			break;
		case 2:
			ee_compare(ee);
			if (ee->error)
				return;

			ee->ival = (oival != ee->ival) ? 1 : 0;
			break;
		}

		ee_skip_spaces(ee);
	}
}

static void ee_bitand(struct expreval *ee)
{
	ee_equals(ee);
	if (!IS_ENABLED(CONFIG_LIL_FULL))
		return;

	ee_skip_spaces(ee);
	while (ee->head < ee->len && !ee->error &&
	       (ee->code[ee->head] == '&' &&
		!ee_invalidpunct(ee->code[ee->head + 1]))) {
		ssize_t oival = ee->ival;
		ee->head++;

		ee_equals(ee);
		if (ee->error)
			return;

		ee->ival = oival & ee->ival;

		ee_skip_spaces(ee);
	}
}

static void ee_bitor(struct expreval *ee)
{
	ee_bitand(ee);
	if (!IS_ENABLED(CONFIG_LIL_FULL))
		return;

	ee_skip_spaces(ee);
	while (ee->head < ee->len && !ee->error &&
	       (ee->code[ee->head] == '|' &&
		!ee_invalidpunct(ee->code[ee->head + 1]))) {
		ssize_t oival = ee->ival;

		ee->head++;

		ee_bitand(ee);
		if (ee->error)
			return;

		ee->ival = oival | ee->ival;

		ee_skip_spaces(ee);
	}
}

static void ee_logand(struct expreval *ee)
{
	ee_bitor(ee);
	ee_skip_spaces(ee);

	while (ee->head < ee->len && !ee->error &&
	       (ee->code[ee->head] == '&' && ee->code[ee->head + 1] == '&')) {
		ssize_t oival = ee->ival;

		ee->head += 2;

		ee_bitor(ee);
		if (ee->error)
			return;

		ee->ival = (oival && ee->ival) ? 1 : 0;

		ee_skip_spaces(ee);
	}
}

static void ee_logor(struct expreval *ee)
{
	ee_logand(ee);
	ee_skip_spaces(ee);

	while (ee->head < ee->len && !ee->error &&
	       (ee->code[ee->head] == '|' && ee->code[ee->head + 1] == '|')) {
		ssize_t oival = ee->ival;

		ee->head += 2;

		ee_logand(ee);
		if (ee->error)
			return;

		ee->ival = (oival || ee->ival) ? 1 : 0;

		ee_skip_spaces(ee);
	}
}

static void ee_expr(struct expreval *ee)
{
	ee_logor(ee);
	if (ee->error == EERR_INVALID_EXPRESSION) {
		/*
		 * invalid expression doesn't really matter, it is only used to
		 * stop the expression parsing.
		 */
		ee->error = EERR_NO_ERROR;
		ee->ival = 1;
	}
}

struct lil_value *lil_eval_expr(struct lil *lil, struct lil_value *code)
{
	struct expreval ee;

	if (ctrlc()) {
		lil_set_error(lil, "interrupted");
		return NULL;
	}

	code = lil_subst_to_value(lil, code);
	if (lil->error)
		return NULL;

	ee.code = lil_to_string(code);
	if (!ee.code[0]) {
		/*
		 * an empty expression equals to 0 so that it can be used as a
		 * false value in conditionals
		 */
		lil_free_value(code);
		return lil_alloc_integer(0);
	}

	ee.head = 0;
	ee.len = code->l;
	ee.ival = 0;
	ee.error = 0;

	ee_expr(&ee);
	lil_free_value(code);
	if (ee.error) {
		switch (ee.error) {
		case EERR_DIVISION_BY_ZERO:
			lil_set_error(lil, "division by zero in expression");
			break;
		case EERR_SYNTAX_ERROR:
			lil_set_error(lil, "expression syntax error");
			break;
		}
		return NULL;
	}
	return lil_alloc_integer(ee.ival);
}

struct lil_value *lil_unused_name(struct lil *lil, const char *part)
{
	char *name = malloc(strlen(part) + 64);
	struct lil_value *val;
	size_t i;

	for (i = 0; i < (size_t)-1; i++) {
		sprintf(name, "!!un!%s!%09u!nu!!", part, (unsigned int)i);
		if (lil_find_cmd(lil, name))
			continue;

		if (lil_find_var(lil, lil->env, name))
			continue;

		val = lil_alloc_string(name);
		free(name);
		return val;
	}
	return NULL;
}

struct lil_value *lil_arg(struct lil_value **argv, size_t index)
{
	return argv ? argv[index] : NULL;
}

const char *lil_to_string(struct lil_value *val)
{
	return (val && val->l) ? val->d : "";
}

ssize_t lil_to_integer(struct lil_value *val)
{
	return simple_strtol(lil_to_string(val), NULL, 0);
}

int lil_to_boolean(struct lil_value *val)
{
	const char *s = lil_to_string(val);
	size_t i, dots = 0;

	if (!s[0])
		return 0;

	for (i = 0; s[i]; i++) {
		if (s[i] != '0' && s[i] != '.')
			return 1;

		if (s[i] == '.') {
			if (dots)
				return 1;

			dots = 1;
		}
	}

	return 0;
}

struct lil_value *lil_alloc_string(const char *str)
{
	return alloc_value(str);
}

struct lil_value *lil_alloc_string_len(const char *str, size_t len)
{
	return alloc_value_len(str, len);
}

struct lil_value *lil_alloc_integer(ssize_t num)
{
	char buff[128];

	sprintf(buff, "%zd", num);
	return alloc_value(buff);
}

void lil_free(struct lil *lil)
{
	size_t i;

	if (!lil)
		return;

	free(lil->err_msg);
	lil_free_value(lil->empty);
	while (lil->env) {
		struct lil_env *next = lil->env->parent;

		lil_free_env(lil->env);
		lil->env = next;
	}

	for (i = 0; i < lil->cmds; i++) {
		if (lil->cmd[i]->argnames)
			lil_free_list(lil->cmd[i]->argnames);

		lil_free_value(lil->cmd[i]->code);
		free(lil->cmd[i]->name);
		free(lil->cmd[i]);
	}

	hm_destroy(&lil->cmdmap);
	free(lil->cmd);
	free(lil->dollarprefix);
	free(lil->catcher);
	free(lil);
}

static struct lil_value *fnc_reflect(struct lil *lil, size_t argc,
				     struct lil_value **argv)
{
	struct lil_func *func;
	const char *type;
	size_t i;
	struct lil_value *r;

	if (!argc)
		return NULL;

	type = lil_to_string(argv[0]);
	if (!strcmp(type, "version"))
		return lil_alloc_string(LIL_VERSION_STRING);

	if (!strcmp(type, "args")) {
		if (argc < 2)
			return NULL;
		func = lil_find_cmd(lil, lil_to_string(argv[1]));
		if (!func || !func->argnames)
			return NULL;
		return lil_list_to_value(func->argnames, 1);
	}

	if (!strcmp(type, "body")) {
		if (argc < 2)
			return NULL;

		func = lil_find_cmd(lil, lil_to_string(argv[1]));
		if (!func || func->proc)
			return NULL;

		return lil_clone_value(func->code);
	}

	if (!strcmp(type, "func-count"))
		return lil_alloc_integer(lil->cmds);

	if (!strcmp(type, "funcs")) {
		struct lil_list *funcs = lil_alloc_list();

		for (i = 0; i < lil->cmds; i++)
			lil_list_append(funcs,
					lil_alloc_string(lil->cmd[i]->name));

		r = lil_list_to_value(funcs, 1);
		lil_free_list(funcs);
		return r;
	}

	if (!strcmp(type, "vars")) {
		struct lil_list *vars = lil_alloc_list();
		struct lil_env *env = lil->env;

		while (env) {
			for (i = 0; i < env->vars; i++)
				lil_list_append(
					vars, lil_alloc_string(env->var[i]->n));
			env = env->parent;
		}

		r = lil_list_to_value(vars, 1);
		lil_free_list(vars);
		return r;
	}

	if (!strcmp(type, "globals")) {
		struct lil_list *vars = lil_alloc_list();

		for (i = 0; i < lil->rootenv->vars; i++)
			lil_list_append(vars, lil_alloc_string(
						      lil->rootenv->var[i]->n));

		r = lil_list_to_value(vars, 1);
		lil_free_list(vars);
		return r;
	}

	if (!strcmp(type, "has-func")) {
		const char *target;

		if (argc == 1)
			return NULL;

		target = lil_to_string(argv[1]);
		return hm_has(&lil->cmdmap, target) ? lil_alloc_string("1") :
							    NULL;
	}

	if (!strcmp(type, "has-var")) {
		const char *target;
		struct lil_env *env = lil->env;

		if (argc == 1)
			return NULL;

		target = lil_to_string(argv[1]);
		while (env) {
			if (hm_has(&env->varmap, target))
				return lil_alloc_string("1");
			env = env->parent;
		}
		return NULL;
	}

	if (!strcmp(type, "has-global")) {
		const char *target;

		if (argc == 1)
			return NULL;

		target = lil_to_string(argv[1]);
		for (i = 0; i < lil->rootenv->vars; i++)
			if (!strcmp(target, lil->rootenv->var[i]->n))
				return lil_alloc_string("1");
		return NULL;
	}

	if (!strcmp(type, "error"))
		return lil->err_msg ? lil_alloc_string(lil->err_msg) : NULL;

	if (!strcmp(type, "dollar-prefix")) {
		struct lil_value *r;

		if (argc == 1)
			return lil_alloc_string(lil->dollarprefix);

		r = lil_alloc_string(lil->dollarprefix);
		free(lil->dollarprefix);
		lil->dollarprefix = strdup(lil_to_string(argv[1]));
		return r;
	}

	if (!strcmp(type, "this")) {
		struct lil_env *env = lil->env;

		while (env != lil->rootenv && !env->catcher_for && !env->func)
			env = env->parent;

		if (env->catcher_for)
			return lil_alloc_string(lil->catcher);

		if (env == lil->rootenv)
			return lil_alloc_string(lil->rootcode);

		return env->func ? env->func->code : NULL;
	}

	if (!strcmp(type, "name")) {
		struct lil_env *env = lil->env;

		while (env != lil->rootenv && !env->catcher_for && !env->func)
			env = env->parent;

		if (env->catcher_for)
			return env->catcher_for;

		if (env == lil->rootenv)
			return NULL;

		return env->func ? lil_alloc_string(env->func->name) : NULL;
	}

	return NULL;
}

static struct lil_value *fnc_func(struct lil *lil, size_t argc,
				  struct lil_value **argv)
{
	struct lil_value *name;
	struct lil_func *cmd;
	struct lil_list *fargs;

	if (argc < 1)
		return NULL;

	if (argc >= 3) {
		name = lil_clone_value(argv[0]);
		fargs = lil_subst_to_list(lil, argv[1]);
		cmd = add_func(lil, lil_to_string(argv[0]));
		cmd->argnames = fargs;
		cmd->code = lil_clone_value(argv[2]);
	} else {
		name = lil_unused_name(lil, "anonymous-function");
		if (argc < 2) {
			struct lil_value *tmp = lil_alloc_string("args");

			fargs = lil_subst_to_list(lil, tmp);
			lil_free_value(tmp);
			cmd = add_func(lil, lil_to_string(name));
			cmd->argnames = fargs;
			cmd->code = lil_clone_value(argv[0]);
		} else {
			fargs = lil_subst_to_list(lil, argv[0]);
			cmd = add_func(lil, lil_to_string(name));
			cmd->argnames = fargs;
			cmd->code = lil_clone_value(argv[1]);
		}
	}

	return name;
}

static struct lil_value *fnc_rename(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	struct lil_value *r;
	struct lil_func *func;
	const char *oldname;
	const char *newname;

	if (argc < 2)
		return NULL;

	oldname = lil_to_string(argv[0]);
	newname = lil_to_string(argv[1]);
	func = lil_find_cmd(lil, oldname);
	if (!func) {
		lil_set_errorf_at(lil, lil->head, "unknown function '%s'",
				  oldname);
		return NULL;
	}

	r = lil_alloc_string(func->name);
	if (newname[0]) {
		hm_put(&lil->cmdmap, oldname, 0);
		hm_put(&lil->cmdmap, newname, func);
		free(func->name);
		func->name = strdup(newname);
	} else {
		del_func(lil, func);
	}

	return r;
}

static struct lil_value *fnc_unusedname(struct lil *lil, size_t argc,
					struct lil_value **argv)
{
	return lil_unused_name(lil, argc > 0 ? lil_to_string(argv[0]) :
						     "unusedname");
}

static struct lil_value *fnc_quote(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	struct lil_value *r;
	size_t i;

	if (argc < 1)
		return NULL;

	r = alloc_value(NULL);
	for (i = 0; i < argc; i++) {
		if (i)
			lil_append_char(r, ' ');
		lil_append_val(r, argv[i]);
	}

	return r;
}

static struct lil_value *fnc_set(struct lil *lil, size_t argc,
				 struct lil_value **argv)
{
	size_t i = 0;
	struct lil_var *var = NULL;
	int access = LIL_SETVAR_LOCAL;

	if (!argc)
		return NULL;

	if (!strcmp(lil_to_string(argv[0]), "global")) {
		i = 1;
		access = LIL_SETVAR_GLOBAL;
	}

	while (i < argc) {
		if (argc == i + 1)
			return lil_clone_value(
				lil_get_var(lil, lil_to_string(argv[i])));

		var = lil_set_var(lil, lil_to_string(argv[i]), argv[i + 1],
				  access);
		i += 2;
	}

	return var ? lil_clone_value(var->v) : NULL;
}

static struct lil_value *fnc_local(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	size_t i;

	for (i = 0; i < argc; i++) {
		const char *varname = lil_to_string(argv[i]);

		if (!lil_find_local_var(lil, lil->env, varname))
			lil_set_var(lil, varname, lil->empty,
				    LIL_SETVAR_LOCAL_NEW);
	}

	return NULL;
}

static struct lil_value *fnc_eval(struct lil *lil, size_t argc,
				  struct lil_value **argv)
{
	if (argc == 1)
		return lil_parse_value(lil, argv[0], 0);

	if (argc > 1) {
		struct lil_value *val = alloc_value(NULL), *r;
		size_t i;

		for (i = 0; i < argc; i++) {
			if (i)
				lil_append_char(val, ' ');
			lil_append_val(val, argv[i]);
		}

		r = lil_parse_value(lil, val, 0);
		lil_free_value(val);
		return r;
	}

	return NULL;
}

static struct lil_value *fnc_topeval(struct lil *lil, size_t argc,
				     struct lil_value **argv)
{
	struct lil_env *thisenv = lil->env;
	struct lil_env *thisdownenv = lil->downenv;
	struct lil_value *r;

	lil->env = lil->rootenv;
	lil->downenv = thisenv;

	r = fnc_eval(lil, argc, argv);
	lil->downenv = thisdownenv;
	lil->env = thisenv;
	return r;
}

static struct lil_value *fnc_upeval(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	struct lil_env *thisenv = lil->env;
	struct lil_env *thisdownenv = lil->downenv;
	struct lil_value *r;

	if (lil->rootenv == thisenv)
		return fnc_eval(lil, argc, argv);

	lil->env = thisenv->parent;
	lil->downenv = thisenv;

	r = fnc_eval(lil, argc, argv);
	lil->env = thisenv;
	lil->downenv = thisdownenv;
	return r;
}

static struct lil_value *fnc_downeval(struct lil *lil, size_t argc,
				      struct lil_value **argv)
{
	struct lil_value *r;
	struct lil_env *upenv = lil->env;
	struct lil_env *downenv = lil->downenv;

	if (!downenv)
		return fnc_eval(lil, argc, argv);

	lil->downenv = NULL;
	lil->env = downenv;

	r = fnc_eval(lil, argc, argv);
	lil->downenv = downenv;
	lil->env = upenv;
	return r;
}

static struct lil_value *fnc_enveval(struct lil *lil, size_t argc,
				     struct lil_value **argv)
{
	struct lil_value *r;
	struct lil_list *invars = NULL;
	struct lil_list *outvars = NULL;
	struct lil_value **varvalues = NULL;
	int codeindex;
	size_t i;

	if (argc < 1)
		return NULL;

	if (argc == 1) {
		codeindex = 0;
	} else if (argc >= 2) {
		invars = lil_subst_to_list(lil, argv[0]);
		varvalues = malloc(sizeof(struct lil_value *) *
				   lil_list_size(invars));

		for (i = 0; i < lil_list_size(invars); i++)
			varvalues[i] = lil_clone_value(lil_get_var(
				lil, lil_to_string(lil_list_get(invars, i))));

		if (argc > 2) {
			codeindex = 2;
			outvars = lil_subst_to_list(lil, argv[1]);
		} else {
			codeindex = 1;
		}
	}

	lil_push_env(lil);
	if (invars) {
		for (i = 0; i < lil_list_size(invars); i++) {
			lil_set_var(lil, lil_to_string(lil_list_get(invars, i)),
				    varvalues[i], LIL_SETVAR_LOCAL_NEW);
			lil_free_value(varvalues[i]);
		}
	}

	r = lil_parse_value(lil, argv[codeindex], 0);

	if (outvars) {
		varvalues = realloc(varvalues, sizeof(struct lil_value *) *
						       lil_list_size(outvars));

		for (i = 0; i < lil_list_size(outvars); i++)
			varvalues[i] = lil_clone_value(lil_get_var(
				lil, lil_to_string(lil_list_get(outvars, i))));
	} else if (invars) {
		for (i = 0; i < lil_list_size(invars); i++)
			varvalues[i] = lil_clone_value(lil_get_var(
				lil, lil_to_string(lil_list_get(invars, i))));
	}

	lil_pop_env(lil);
	if (invars) {
		if (outvars) {
			for (i = 0; i < lil_list_size(outvars); i++) {
				lil_set_var(
					lil,
					lil_to_string(lil_list_get(outvars, i)),
					varvalues[i], LIL_SETVAR_LOCAL);
				lil_free_value(varvalues[i]);
			}
		} else {
			for (i = 0; i < lil_list_size(invars); i++) {
				lil_set_var(
					lil,
					lil_to_string(lil_list_get(invars, i)),
					varvalues[i], LIL_SETVAR_LOCAL);
				lil_free_value(varvalues[i]);
			}
		}

		lil_free_list(invars);
		if (outvars)
			lil_free_list(outvars);
		free(varvalues);
	}

	return r;
}

static struct lil_value *fnc_jaileval(struct lil *lil, size_t argc,
				      struct lil_value **argv)
{
	size_t i;
	struct lil *sublil;
	struct lil_value *r;
	size_t base = 0;

	if (!argc)
		return NULL;

	if (!strcmp(lil_to_string(argv[0]), "clean")) {
		base = 1;
		if (argc == 1)
			return NULL;
	}

	sublil = lil_new();
	if (base != 1) {
		for (i = lil->syscmds; i < lil->cmds; i++) {
			struct lil_func *fnc = lil->cmd[i];
			if (!fnc->proc)
				continue;
			lil_register(sublil, fnc->name, fnc->proc);
		}
	}

	r = lil_parse_value(sublil, argv[base], 1);
	lil_free(sublil);
	return r;
}

static struct lil_value *fnc_count(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	struct lil_list *list;
	char buff[64];

	if (!argc)
		return alloc_value("0");

	list = lil_subst_to_list(lil, argv[0]);
	sprintf(buff, "%u", (unsigned int)list->c);
	lil_free_list(list);
	return alloc_value(buff);
}

static struct lil_value *fnc_index(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	struct lil_list *list;
	size_t index;
	struct lil_value *r;

	if (argc < 2)
		return NULL;

	list = lil_subst_to_list(lil, argv[0]);
	index = (size_t)lil_to_integer(argv[1]);
	if (index >= list->c)
		r = NULL;
	else
		r = lil_clone_value(list->v[index]);
	lil_free_list(list);
	return r;
}

static struct lil_value *fnc_indexof(struct lil *lil, size_t argc,
				     struct lil_value **argv)
{
	struct lil_list *list;
	size_t index;
	struct lil_value *r = NULL;
	if (argc < 2)
		return NULL;

	list = lil_subst_to_list(lil, argv[0]);
	for (index = 0; index < list->c; index++) {
		if (!strcmp(lil_to_string(list->v[index]),
			    lil_to_string(argv[1]))) {
			r = lil_alloc_integer(index);
			break;
		}
	}

	lil_free_list(list);
	return r;
}

static struct lil_value *fnc_append(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	struct lil_list *list;
	struct lil_value *r;
	size_t i, base = 1;
	int access = LIL_SETVAR_LOCAL;
	const char *varname;

	if (argc < 2)
		return NULL;

	varname = lil_to_string(argv[0]);
	if (!strcmp(varname, "global")) {
		if (argc < 3)
			return NULL;

		varname = lil_to_string(argv[1]);
		base = 2;
		access = LIL_SETVAR_GLOBAL;
	}

	list = lil_subst_to_list(lil, lil_get_var(lil, varname));
	for (i = base; i < argc; i++)
		lil_list_append(list, lil_clone_value(argv[i]));

	r = lil_list_to_value(list, 1);
	lil_free_list(list);
	lil_set_var(lil, varname, r, access);
	return r;
}

static struct lil_value *fnc_slice(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	struct lil_list *list, *slice;
	size_t i;
	ssize_t from, to;
	struct lil_value *r;

	if (argc < 1)
		return NULL;
	if (argc < 2)
		return lil_clone_value(argv[0]);

	from = lil_to_integer(argv[1]);
	if (from < 0)
		from = 0;

	list = lil_subst_to_list(lil, argv[0]);
	to = argc > 2 ? lil_to_integer(argv[2]) : (ssize_t)list->c;
	if (to > (ssize_t)list->c)
		to = list->c;
	else if (to < from)
		to = from;

	slice = lil_alloc_list();
	for (i = (size_t)from; i < (size_t)to; i++)
		lil_list_append(slice, lil_clone_value(list->v[i]));
	lil_free_list(list);

	r = lil_list_to_value(slice, 1);
	lil_free_list(slice);
	return r;
}

static struct lil_value *fnc_filter(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	struct lil_list *list, *filtered;
	size_t i;
	struct lil_value *r;
	const char *varname = "x";
	int base = 0;

	if (argc < 1)
		return NULL;

	if (argc < 2)
		return lil_clone_value(argv[0]);

	if (argc > 2) {
		base = 1;
		varname = lil_to_string(argv[0]);
	}

	list = lil_subst_to_list(lil, argv[base]);
	filtered = lil_alloc_list();
	for (i = 0; i < list->c && !lil->env->breakrun; i++) {
		lil_set_var(lil, varname, list->v[i], LIL_SETVAR_LOCAL_ONLY);
		r = lil_eval_expr(lil, argv[base + 1]);
		if (lil_to_boolean(r))
			lil_list_append(filtered, lil_clone_value(list->v[i]));
		lil_free_value(r);
	}
	lil_free_list(list);

	r = lil_list_to_value(filtered, 1);
	lil_free_list(filtered);
	return r;
}

static struct lil_value *fnc_list(struct lil *lil, size_t argc,
				  struct lil_value **argv)
{
	struct lil_list *list = lil_alloc_list();
	struct lil_value *r;
	size_t i;

	for (i = 0; i < argc; i++)
		lil_list_append(list, lil_clone_value(argv[i]));

	r = lil_list_to_value(list, 1);
	lil_free_list(list);
	return r;
}

static struct lil_value *fnc_subst(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	if (argc < 1)
		return NULL;

	return lil_subst_to_value(lil, argv[0]);
}

static struct lil_value *fnc_concat(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	struct lil_list *list;
	struct lil_value *r, *tmp;
	size_t i;

	if (argc < 1)
		return NULL;

	r = lil_alloc_string("");
	for (i = 0; i < argc; i++) {
		list = lil_subst_to_list(lil, argv[i]);
		tmp = lil_list_to_value(list, 1);
		lil_free_list(list);
		lil_append_val(r, tmp);
		lil_free_value(tmp);
	}
	return r;
}

static struct lil_value *fnc_foreach(struct lil *lil, size_t argc,
				     struct lil_value **argv)
{
	struct lil_list *list, *rlist;
	struct lil_value *r;
	size_t i, listidx = 0, codeidx = 1;
	const char *varname = "i";

	if (argc < 2)
		return NULL;

	if (argc >= 3) {
		varname = lil_to_string(argv[0]);
		listidx = 1;
		codeidx = 2;
	}

	rlist = lil_alloc_list();
	list = lil_subst_to_list(lil, argv[listidx]);
	for (i = 0; i < list->c; i++) {
		struct lil_value *rv;

		lil_set_var(lil, varname, list->v[i], LIL_SETVAR_LOCAL_ONLY);
		rv = lil_parse_value(lil, argv[codeidx], 0);
		if (rv->l)
			lil_list_append(rlist, rv);
		else
			lil_free_value(rv);

		if (lil->env->breakrun || lil->error)
			break;
	}

	r = lil_list_to_value(rlist, 1);
	lil_free_list(list);
	lil_free_list(rlist);
	return r;
}

static struct lil_value *fnc_return(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	lil->env->breakrun = 1;
	lil_free_value(lil->env->retval);
	lil->env->retval = argc < 1 ? NULL : lil_clone_value(argv[0]);
	lil->env->retval_set = 1;
	return argc < 1 ? NULL : lil_clone_value(argv[0]);
}

static struct lil_value *fnc_result(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	if (argc > 0) {
		lil_free_value(lil->env->retval);
		lil->env->retval = lil_clone_value(argv[0]);
		lil->env->retval_set = 1;
	}

	return lil->env->retval_set ? lil_clone_value(lil->env->retval) : NULL;
}

static struct lil_value *fnc_expr(struct lil *lil, size_t argc,
				  struct lil_value **argv)
{
	if (argc == 1)
		return lil_eval_expr(lil, argv[0]);

	if (argc > 1) {
		struct lil_value *val = alloc_value(NULL), *r;
		size_t i;

		for (i = 0; i < argc; i++) {
			if (i)
				lil_append_char(val, ' ');
			lil_append_val(val, argv[i]);
		}

		r = lil_eval_expr(lil, val);
		lil_free_value(val);
		return r;
	}

	return NULL;
}

static struct lil_value *real_inc(struct lil *lil, const char *varname,
				  ssize_t v)
{
	struct lil_value *pv = lil_get_var(lil, varname);

	pv = lil_alloc_integer(lil_to_integer(pv) + v);
	lil_set_var(lil, varname, pv, LIL_SETVAR_LOCAL);
	return pv;
}

static struct lil_value *fnc_inc(struct lil *lil, size_t argc,
				 struct lil_value **argv)
{
	if (argc < 1)
		return NULL;

	return real_inc(lil, lil_to_string(argv[0]),
			argc > 1 ? lil_to_integer(argv[1]) : 1);
}

static struct lil_value *fnc_dec(struct lil *lil, size_t argc,
				 struct lil_value **argv)
{
	if (argc < 1)
		return NULL;

	return real_inc(lil, lil_to_string(argv[0]),
			-(argc > 1 ? lil_to_integer(argv[1]) : 1));
}

static struct lil_value *fnc_if(struct lil *lil, size_t argc,
				struct lil_value **argv)
{
	struct lil_value *val, *r = NULL;
	int base = 0, not = 0, v;

	if (argc < 1)
		return NULL;

	if (!strcmp(lil_to_string(argv[0]), "not"))
		base = not = 1;

	if (argc < (size_t)base + 2)
		return NULL;

	val = lil_eval_expr(lil, argv[base]);
	if (!val || lil->error)
		return NULL;

	v = lil_to_boolean(val);
	if (not )
		v = !v;

	if (v) {
		r = lil_parse_value(lil, argv[base + 1], 0);
	} else if (argc > (size_t)base + 2) {
		r = lil_parse_value(lil, argv[base + 2], 0);
	}

	lil_free_value(val);
	return r;
}

static struct lil_value *fnc_while(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	struct lil_value *val, *r = NULL;
	int base = 0, not = 0, v;

	if (argc < 1)
		return NULL;

	if (!strcmp(lil_to_string(argv[0]), "not"))
		base = not = 1;

	if (argc < (size_t)base + 2)
		return NULL;

	while (!lil->error && !lil->env->breakrun) {
		val = lil_eval_expr(lil, argv[base]);
		if (!val || lil->error)
			return NULL;

		v = lil_to_boolean(val);
		if (not )
			v = !v;

		if (!v) {
			lil_free_value(val);
			break;
		}

		if (r)
			lil_free_value(r);
		r = lil_parse_value(lil, argv[base + 1], 0);
		lil_free_value(val);
	}

	return r;
}

static struct lil_value *fnc_for(struct lil *lil, size_t argc,
				 struct lil_value **argv)
{
	struct lil_value *val, *r = NULL;
	if (argc < 4)
		return NULL;

	lil_free_value(lil_parse_value(lil, argv[0], 0));
	while (!lil->error && !lil->env->breakrun) {
		val = lil_eval_expr(lil, argv[1]);
		if (!val || lil->error)
			return NULL;

		if (!lil_to_boolean(val)) {
			lil_free_value(val);
			break;
		}

		if (r)
			lil_free_value(r);
		r = lil_parse_value(lil, argv[3], 0);
		lil_free_value(val);
		lil_free_value(lil_parse_value(lil, argv[2], 0));
	}

	return r;
}

static struct lil_value *fnc_char(struct lil *lil, size_t argc,
				  struct lil_value **argv)
{
	char s[2];

	if (!argc)
		return NULL;

	s[0] = (char)lil_to_integer(argv[0]);
	s[1] = 0;
	return lil_alloc_string(s);
}

static struct lil_value *fnc_charat(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	size_t index;
	char chstr[2];
	const char *str;

	if (argc < 2)
		return NULL;

	str = lil_to_string(argv[0]);
	index = (size_t)lil_to_integer(argv[1]);
	if (index >= strlen(str))
		return NULL;

	chstr[0] = str[index];
	chstr[1] = 0;
	return lil_alloc_string(chstr);
}

static struct lil_value *fnc_codeat(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	size_t index;
	const char *str;

	if (argc < 2)
		return NULL;

	str = lil_to_string(argv[0]);
	index = (size_t)lil_to_integer(argv[1]);
	if (index >= strlen(str))
		return NULL;

	return lil_alloc_integer(str[index]);
}

static struct lil_value *fnc_substr(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	const char *str;
	struct lil_value *r;
	size_t start, end, i, slen;

	if (argc < 2)
		return NULL;

	str = lil_to_string(argv[0]);
	if (!str[0])
		return NULL;

	slen = strlen(str);
	start = simple_strtol(lil_to_string(argv[1]), NULL, 0);
	end = argc > 2 ? simple_strtol(lil_to_string(argv[2]), NULL, 0) : slen;
	if (end > slen)
		end = slen;

	if (start >= end)
		return NULL;

	r = lil_alloc_string("");
	for (i = start; i < end; i++)
		lil_append_char(r, str[i]);
	return r;
}

static struct lil_value *fnc_strpos(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	const char *hay;
	const char *str;
	size_t min = 0;

	if (argc < 2)
		return lil_alloc_integer(-1);

	hay = lil_to_string(argv[0]);
	if (argc > 2) {
		min = simple_strtol(lil_to_string(argv[2]), NULL, 0);
		if (min >= strlen(hay))
			return lil_alloc_integer(-1);
	}

	str = strstr(hay + min, lil_to_string(argv[1]));
	if (!str)
		return lil_alloc_integer(-1);

	return lil_alloc_integer(str - hay);
}

static struct lil_value *fnc_length(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	size_t i, total = 0;

	for (i = 0; i < argc; i++) {
		if (i)
			total++;
		total += strlen(lil_to_string(argv[i]));
	}

	return lil_alloc_integer((ssize_t)total);
}

static struct lil_value *real_trim(const char *str, const char *chars, int left,
				   int right)
{
	int base = 0;
	struct lil_value *r = NULL;

	if (left) {
		while (str[base] && strchr(chars, str[base]))
			base++;
		if (!right)
			r = lil_alloc_string(str[base] ? str + base : NULL);
	}

	if (right) {
		size_t len;
		char *s;

		s = strdup(str + base);
		len = strlen(s);
		while (len && strchr(chars, s[len - 1]))
			len--;

		s[len] = 0;
		r = lil_alloc_string(s);
		free(s);
	}

	return r;
}

static struct lil_value *fnc_trim(struct lil *lil, size_t argc,
				  struct lil_value **argv)
{
	const char *chars;

	if (!argc)
		return NULL;

	if (argc < 2)
		chars = " \f\n\r\t\v";
	else
		chars = lil_to_string(argv[1]);

	return real_trim(lil_to_string(argv[0]), chars, 1, 1);
}

static struct lil_value *fnc_ltrim(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	const char *chars;

	if (!argc)
		return NULL;

	if (argc < 2)
		chars = " \f\n\r\t\v";
	else
		chars = lil_to_string(argv[1]);

	return real_trim(lil_to_string(argv[0]), chars, 1, 0);
}

static struct lil_value *fnc_rtrim(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	const char *chars;

	if (!argc)
		return NULL;

	if (argc < 2)
		chars = " \f\n\r\t\v";
	else
		chars = lil_to_string(argv[1]);

	return real_trim(lil_to_string(argv[0]), chars, 0, 1);
}

static struct lil_value *fnc_strcmp(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	if (argc < 2)
		return NULL;

	return lil_alloc_integer(
		strcmp(lil_to_string(argv[0]), lil_to_string(argv[1])));
}

static struct lil_value *fnc_streq(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	if (argc < 2)
		return NULL;

	return lil_alloc_integer(
		strcmp(lil_to_string(argv[0]), lil_to_string(argv[1])) ? 0 : 1);
}

static struct lil_value *fnc_repstr(struct lil *lil, size_t argc,
				    struct lil_value **argv)
{
	const char *from;
	const char *to;
	char *src;
	const char *sub;
	size_t idx;
	size_t fromlen;
	size_t tolen;
	size_t srclen;
	struct lil_value *r;

	if (argc < 1)
		return NULL;

	if (argc < 3)
		return lil_clone_value(argv[0]);

	from = lil_to_string(argv[1]);
	to = lil_to_string(argv[2]);
	if (!from[0])
		return NULL;

	src = strdup(lil_to_string(argv[0]));
	srclen = strlen(src);
	fromlen = strlen(from);
	tolen = strlen(to);
	while ((sub = strstr(src, from))) {
		char *newsrc = malloc(srclen - fromlen + tolen + 1);

		idx = sub - src;
		if (idx)
			memcpy(newsrc, src, idx);

		memcpy(newsrc + idx, to, tolen);
		memcpy(newsrc + idx + tolen, src + idx + fromlen,
		       srclen - idx - fromlen);
		srclen = srclen - fromlen + tolen;
		free(src);
		src = newsrc;
		src[srclen] = 0;
	}

	r = lil_alloc_string(src);
	free(src);
	return r;
}

static struct lil_value *fnc_split(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	struct lil_list *list;
	const char *sep = " ";
	size_t i;
	struct lil_value *val;
	const char *str;

	if (argc == 0)
		return NULL;
	if (argc > 1) {
		sep = lil_to_string(argv[1]);
		if (!sep || !sep[0])
			return lil_clone_value(argv[0]);
	}

	val = lil_alloc_string("");
	str = lil_to_string(argv[0]);
	list = lil_alloc_list();
	for (i = 0; str[i]; i++) {
		if (strchr(sep, str[i])) {
			lil_list_append(list, val);
			val = lil_alloc_string("");
		} else {
			lil_append_char(val, str[i]);
		}
	}

	lil_list_append(list, val);
	val = lil_list_to_value(list, 1);
	lil_free_list(list);
	return val;
}

static struct lil_value *fnc_try(struct lil *lil, size_t argc,
				 struct lil_value **argv)
{
	struct lil_value *r;
	if (argc < 1)
		return NULL;

	if (lil->error)
		return NULL;

	r = lil_parse_value(lil, argv[0], 0);
	if (lil->error) {
		lil->error = ERROR_NOERROR;
		lil_free_value(r);

		if (argc > 1)
			r = lil_parse_value(lil, argv[1], 0);
		else
			r = 0;
	}
	return r;
}

static struct lil_value *fnc_error(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	lil_set_error(lil, argc > 0 ? lil_to_string(argv[0]) : NULL);
	return NULL;
}

static struct lil_value *fnc_lmap(struct lil *lil, size_t argc,
				  struct lil_value **argv)
{
	struct lil_list *list;
	size_t i;
	if (argc < 2)
		return NULL;

	list = lil_subst_to_list(lil, argv[0]);
	for (i = 1; i < argc; i++)
		lil_set_var(lil, lil_to_string(argv[i]),
			    lil_list_get(list, i - 1), LIL_SETVAR_LOCAL);
	lil_free_list(list);
	return NULL;
}

static struct lil_value *fnc_catcher(struct lil *lil, size_t argc,
				     struct lil_value **argv)
{
	if (argc == 0) {
		return lil_alloc_string(lil->catcher);
	} else {
		const char *catcher = lil_to_string(argv[0]);

		free(lil->catcher);
		lil->catcher = catcher[0] ? strdup(catcher) : NULL;
		return NULL;
	}
}

static struct lil_value *fnc_watch(struct lil *lil, size_t argc,
				   struct lil_value **argv)
{
	size_t i;
	const char *wcode;

	if (argc < 2)
		return NULL;

	wcode = lil_to_string(argv[argc - 1]);
	for (i = 0; i + 1 < argc; i++) {
		const char *vname = lil_to_string(argv[i]);
		struct lil_var *v;

		if (!vname[0])
			continue;

		v = lil_find_var(lil, lil->env, lil_to_string(argv[i]));
		if (!v)
			v = lil_set_var(lil, vname, NULL, LIL_SETVAR_LOCAL_NEW);

		free(v->w);
		v->w = wcode[0] ? strdup(wcode) : NULL;
	}

	return NULL;
}

static void register_stdcmds(struct lil *lil)
{
	lil_register(lil, "dec", fnc_dec);
	lil_register(lil, "eval", fnc_eval);
	lil_register(lil, "expr", fnc_expr);
	lil_register(lil, "for", fnc_for);
	lil_register(lil, "foreach", fnc_foreach);
	lil_register(lil, "func", fnc_func);
	lil_register(lil, "if", fnc_if);
	lil_register(lil, "inc", fnc_inc);
	lil_register(lil, "local", fnc_local);
	lil_register(lil, "return", fnc_return);
	lil_register(lil, "set", fnc_set);
	lil_register(lil, "strcmp", fnc_strcmp);
	lil_register(lil, "try", fnc_try);
	lil_register(lil, "while", fnc_while);

	if (IS_ENABLED(CONFIG_LIL_FULL)) {
		lil_register(lil, "append", fnc_append);
		lil_register(lil, "catcher", fnc_catcher);
		lil_register(lil, "char", fnc_char);
		lil_register(lil, "charat", fnc_charat);
		lil_register(lil, "codeat", fnc_codeat);
		lil_register(lil, "concat", fnc_concat);
		lil_register(lil, "count", fnc_count);
		lil_register(lil, "downeval", fnc_downeval);
		lil_register(lil, "enveval", fnc_enveval);
		lil_register(lil, "error", fnc_error);
		lil_register(lil, "filter", fnc_filter);
		lil_register(lil, "index", fnc_index);
		lil_register(lil, "indexof", fnc_indexof);
		lil_register(lil, "jaileval", fnc_jaileval);
		lil_register(lil, "length", fnc_length);
		lil_register(lil, "list", fnc_list);
		lil_register(lil, "lmap", fnc_lmap);
		lil_register(lil, "ltrim", fnc_ltrim);
		lil_register(lil, "quote", fnc_quote);
		lil_register(lil, "reflect", fnc_reflect);
		lil_register(lil, "rename", fnc_rename);
		lil_register(lil, "repstr", fnc_repstr);
		lil_register(lil, "result", fnc_result);
		lil_register(lil, "rtrim", fnc_rtrim);
		lil_register(lil, "slice", fnc_slice);
		lil_register(lil, "split", fnc_split);
		lil_register(lil, "streq", fnc_streq);
		lil_register(lil, "strpos", fnc_strpos);
		lil_register(lil, "subst", fnc_subst);
		lil_register(lil, "substr", fnc_substr);
		lil_register(lil, "topeval", fnc_topeval);
		lil_register(lil, "trim", fnc_trim);
		lil_register(lil, "unusedname", fnc_unusedname);
		lil_register(lil, "upeval", fnc_upeval);
		lil_register(lil, "watch", fnc_watch);
	}

	lil->syscmds = lil->cmds;
}
