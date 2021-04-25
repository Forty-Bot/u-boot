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

enum lil_setvar {
	LIL_SETVAR_GLOBAL = 0,
	LIL_SETVAR_LOCAL,
	LIL_SETVAR_LOCAL_NEW,
	LIL_SETVAR_LOCAL_ONLY,
};

#define LIL_CALLBACK_EXIT 0
#define LIL_CALLBACK_WRITE 1
#define LIL_CALLBACK_READ 2
#define LIL_CALLBACK_STORE 3
#define LIL_CALLBACK_SOURCE 4
#define LIL_CALLBACK_ERROR 5
#define LIL_CALLBACK_SETVAR 6
#define LIL_CALLBACK_GETVAR 7

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
typedef void (*lil_exit_callback_proc_t)(struct lil *lil,
					 struct lil_value *arg);
typedef void (*lil_write_callback_proc_t)(struct lil *lil, const char *msg);
typedef char *(*lil_read_callback_proc_t)(struct lil *lil, const char *name);
typedef char *(*lil_source_callback_proc_t)(struct lil *lil, const char *name);
typedef void (*lil_store_callback_proc_t)(struct lil *lil, const char *name,
					  const char *data);
typedef void (*lil_error_callback_proc_t)(struct lil *lil, size_t pos,
					  const char *msg);
typedef int (*lil_setvar_callback_proc_t)(struct lil *lil, const char *name,
					  struct lil_value **value);
typedef int (*lil_getvar_callback_proc_t)(struct lil *lil, const char *name,
					  struct lil_value **value);
typedef void (*lil_callback_proc_t)(void);

struct lil *lil_new(void);
void lil_free(struct lil *lil);

int lil_register(struct lil *lil, const char *name, lil_func_proc_t proc);

struct lil_value *lil_parse(struct lil *lil, const char *code, size_t codelen,
			    int funclevel);
struct lil_value *lil_parse_value(struct lil *lil, struct lil_value *val,
				  int funclevel);
struct lil_value *lil_call(struct lil *lil, const char *funcname, size_t argc,
			   struct lil_value **argv);

void lil_callback(struct lil *lil, int cb, lil_callback_proc_t proc);

void lil_set_error(struct lil *lil, const char *msg);
void lil_set_error_at(struct lil *lil, size_t pos, const char *msg);
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
