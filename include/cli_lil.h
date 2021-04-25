/* SPDX-License-Identifier: GPL-2.0+ AND Zlib */
/*
 * LIL - Little Interpreted Language
 * Copyright (C) 2021 Sean Anderson <seanga2@gmail.com>
 * Copyright (C) 2010-2013 Kostas Michalopoulos <badsector@runtimelegend.com>
 *
 * This file originated from the LIL project, licensed under Zlib
 * All changes licensed under GPL-2.0+
 */

#ifndef __LIL_H_INCLUDED__
#define __LIL_H_INCLUDED__

#define LIL_VERSION_STRING "0.1"

/**
 * enum lil_setvar - The strategy to use when creating new variables
 */
enum lil_setvar {
	/**
	 * @LIL_SETVAR_GLOBAL: Set in the root environment
	 */
	LIL_SETVAR_GLOBAL = 0,
	/**
	 * @LIL_SETVAR_LOCAL: Set, starting with the local environment
	 *
	 * Search for a variable. If one is found, overwrite it. Otherwise,
	 * create a new variable in the local environment.
	 */
	LIL_SETVAR_LOCAL,
	/**
	 * @LIL_SETVAR_LOCAL_NEW: Create in the local environment
	 *
	 * Create a new variable in the local environment. This never overrides
	 * existing variables (even if one exists in the local environment).
	 */
	LIL_SETVAR_LOCAL_NEW,
	/**
	 * @LIL_SETVAR_LOCAL_ONLY: Set in a local environment only
	 *
	 * Search for a variable. If one is found that is not in the root
	 * environment, overwrite it. Otherwise, create a new variable in the
	 * local environment.
	 */
	LIL_SETVAR_LOCAL_ONLY,
};

#include <stdint.h>
#include <inttypes.h>

struct lil_value;
struct lil_func;
struct lil_var;
struct lil_env;
struct lil_list;
struct lil;
typedef struct lil_value *(*lil_func_proc_t)(struct lil *lil, size_t argc,
					     struct lil_value **argv);

struct lil *lil_new(void);
void lil_free(struct lil *lil);

int lil_register(struct lil *lil, const char *name, lil_func_proc_t proc);

struct lil_value *lil_parse(struct lil *lil, const char *code, size_t codelen,
			    int funclevel);
struct lil_value *lil_parse_value(struct lil *lil, struct lil_value *val,
				  int funclevel);
struct lil_value *lil_call(struct lil *lil, const char *funcname, size_t argc,
			   struct lil_value **argv);

int lil_error(struct lil *lil, const char **msg, size_t *pos);

const char *lil_to_string(struct lil_value *val);
ssize_t lil_to_integer(struct lil_value *val);
int lil_to_boolean(struct lil_value *val);

struct lil_value *lil_alloc_string(const char *str);
struct lil_value *lil_alloc_integer(ssize_t num);
void lil_free_value(struct lil_value *val);

struct lil_value *lil_clone_value(struct lil_value *src);
int lil_append_char(struct lil_value *val, char ch);
int lil_append_string(struct lil_value *val, const char *s);
int lil_append_val(struct lil_value *val, struct lil_value *v);

struct lil_list *lil_alloc_list(void);
void lil_free_list(struct lil_list *list);
void lil_list_append(struct lil_list *list, struct lil_value *val);
size_t lil_list_size(struct lil_list *list);
struct lil_value *lil_list_get(struct lil_list *list, size_t index);
struct lil_value *lil_list_to_value(struct lil_list *list, int do_escape);

struct lil_list *lil_subst_to_list(struct lil *lil, struct lil_value *code);
struct lil_value *lil_subst_to_value(struct lil *lil, struct lil_value *code);

struct lil_env *lil_alloc_env(struct lil_env *parent);
void lil_free_env(struct lil_env *env);
struct lil_env *lil_push_env(struct lil *lil);
void lil_pop_env(struct lil *lil);

struct lil_var *lil_set_var(struct lil *lil, const char *name,
			    struct lil_value *val, enum lil_setvar local);
struct lil_value *lil_get_var(struct lil *lil, const char *name);
struct lil_value *lil_get_var_or(struct lil *lil, const char *name,
				 struct lil_value *defvalue);

struct lil_value *lil_eval_expr(struct lil *lil, struct lil_value *code);
struct lil_value *lil_unused_name(struct lil *lil, const char *part);

struct lil_value *lil_arg(struct lil_value **argv, size_t index);

#endif
