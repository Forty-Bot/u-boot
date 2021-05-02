// SPDX-License-Identifier: GPL-2.0+ AND Zlib
/*
 * Copyright (C) 2021 Sean Anderson <seanga2@gmail.com>
 */

#include <common.h>
#include <asm/global_data.h>
#include <cli_lil.h>
#include <test/lib.h>
#include <test/test.h>
#include <test/ut.h>

const char helpers[] =
	"func assert {cond} {"
		"if not [upeval expr [set cond]] {"
			"error [set cond]"
		"}"
	"};"
	"func assert_err {cmd} {"
		"set ok 1;"
		"try {upeval $cmd; set ok 0} {};"
		"assert {$ok};"
	"};"
	"func asserteq {expr1 expr2} {"
		"set val1 [upeval expr $expr1];"
		"set val2 [upeval expr $expr2];"
		"if {$val1 != $val2} {"
			"error '$expr1 == ${expr2}: "
				"Expected ${val1}, got $val2';"
		"}"
	"};"
	/*
	 * We need to use set explicitly to avoid problems if the test redefines
	 * dollar
	 */
	"func asserteq_str {expr1 expr2} {"
		"set val1 [upeval 'subst \"[set expr1]\"'];"
		"set val2 [upeval 'subst \"[set expr2]\"'];"
		"if not [streq [set val1] [set val2]] {"
			"error '[set expr1] == [set expr2]: "
				"Expected [set val1], got [set val2]';"
		"}"
	"};"
	/* TODO: make this like the other asserteq functions */
	"func asserteq_list {xs ys} {"
		"set len [count $xs];"
		"assert {$len == [count $ys]};"
		"for {set i 0} {$i < $len} {inc i} {"
			"assert {[streq [index $xs $i] [index $ys $i]]}"
		"}"
	"}";

static const struct {
	const char *name;
	const char *cmd;
} lil_tests[] = {
	{"and",
		"func and args {"
			"foreach [slice $args 1] {"
				"upeval 'downeval \\'set v \\'\\[${i}\\]';"
				"if not $v { return 0 }"
			"};"
			"return 1"
		"};"
		"set a 0;"
		"set final [and {set a 3} {return 0} {set a 32}];"
		"asserteq 0 {$final};"
		"assert 3 {$a};"
	},
	{"assert",
		"assert 1;"
		"assert_err {assert 0};"
		"asserteq 1 1;"
		"assert_err {asserteq 1 0};"
		"asserteq_str {string one} {string one};"
		"assert_err {asserteq_str {string one} {string two}};"
		"asserteq_list [list 1 2 3] [list 1 2 3];"
		"assert_err {asserteq_list [list 1 2] [list 1 2 3]};"
		"assert_err {asserteq_list [list 1 2 3] [list 1 2]};"
		"assert_err {asserteq_list [list 1 2 3] [list 1 2 4]};"
	},
	{"catcher",
		"catcher {"
			"eval [index $args 2] [index $args 1] [slice $args 3]"
		"};"
		"assert {a streq a};"
	},
	{"dollar",
		"set foo bar baz qux;"
		"asserteq_str bar {$foo};"
		"asserteq_str qux {$baz};"
		"func my-set {name} {"
			"set global last-name [set name];"
			"return [set [set name]]"
		"};"
		"asserteq_str bar {[my-set foo]};"
		"asserteq_str foo {$last-name};"
		"asserteq_str 'set ' {[reflect dollar-prefix]};"
		"reflect dollar-prefix {my-set };"
		"asserteq_str qux {$baz};"
		"asserteq_str baz {[set last-name]}"
	},
	{"downeval",
		"func grab-some-list {} {"
			"set items {};"
			"upeval {"
				"foreach $some-list {"
					"downeval 'append items $i'"
				"}"
			"};"
			"return $items"
		"};"
		"set some-list [list foo bar baz blah moo boo];"
		"asserteq_list $some-list [grab-some-list]"
	},
	{"enveval",
		"func test-vars {} {"
			"local x;"
			"set x 32 y 10 z 88;"
			/*
			 * here both y and z variables will be copied to the new
			 * environment, but only y will be copied back to the
			 * current environment
			 */
			"enveval {y z} {y} {"
				"local x;"
				"asserteq_str '' {$x};"
				"asserteq 10 {$y};"
				"asserteq 88 {$z};"
				"set x 100 y 44 z 123;"
				"asserteq 100 {$x};"
				"asserteq 44 {$y};"
				"asserteq 123 {$z};"
			"};"
			"asserteq 32 {$x};"
			"asserteq 44 {$y};"
			"asserteq 88 {$z}"
		"};"
		"set x 300;"
		"test-vars;"
		"asserteq 300 {$x}"
	},
	{"expr",
		"asserteq 7 {1 + ( 2 * 3 )};"
		"asserteq 7 {1+(2*3)};"
		"asserteq -6 {1+ ~(2*3)};"
		"asserteq -6 {1 + ~( 2 * 3 )};"
		"asserteq -6 {1 +~ (2*3 )};"
		"asserteq -6 {~(2*3)+1};"
		"asserteq 0 {1*!(2+2)};"
		"asserteq -1 {~!(!{})};"
		"asserteq 1 {1 +~*(2*3)};"
		"asserteq 1 {'hello'};"
		"asserteq 0 {0};"
		"asserteq 0 {{}};"
		"asserteq 1 {()};"
		"asserteq 1 {( )};"
		"asserteq_str '' {[expr]};"
	},
	{"filter",
		"set short_funcs [filter [reflect funcs] {[length $x] < 5}];"
		"foreach $short_funcs {assert {[length $i] < 5}}"
	},
	{"funcs",
		"func lapply {list func} {"
			"set ret {};"
			"foreach $list {"
				"append ret [$func $i];"
			"};"
			"return $ret"
		"};"
		"set list [list {bad's day} {good's day} eh??];"
		"asserteq_list [lapply $list split] [list "
			"[list {bad's} day] "
			"[list {good's} day] "
			"[list eh??]"
		"];"
		"asserteq_list [lapply $list length] [list 9 10 4];"
		"asserteq_list [lapply $list [func {a} {"
			"return [index [split $a] 0]"
		"}]] [list {bad's} {good's} eh??]"
	},
	{"jaileval",
		"jaileval {set global foo bar};"
		"assert {![reflect has-var foo]}"
	},
	{"lists",
		"set l [list foo bar baz bad];"
		"asserteq_str baz {[index $l 2]};"
		"append l 'Hello, world!';"
		"asserteq_list $l [list foo bar baz bad 'Hello, world!'];"
		"set l [subst $l];"
		"asserteq_list $l [list foo bar baz bad Hello, world!];"
		"lmap $l foox barx bamia;"
		"asserteq_str foo {$foox};"
		"asserteq_str bar {$barx};"
		"asserteq_str baz {$bamia};"
		"set l {one	# linebreaks are ignored in list parsing mode\n"
		"\n"
		"two;three      # a semicolon still counts as line break\n"
		"               # (which in list mode is treated as a\n"
		"               # separator for list entries)\n"
		"# of course a semicolon inside quotes is treated like normal\n"
		"three';'and';a;half'\n"
		"# like in code mode, a semicolon will stop the comment; four\n"
		"\n"
		"# below we have a quote, square brackets for inline\n"
		"# expansions are still taken into consideration\n"
		"[quote {this line will be ignored completely\n"
		"        as will this line and instead be replaced\n"
		"        with the 'five' below since while in code\n"
		"        mode (that is, inside the brackets here)\n"
		"        linebreaks are still processed}\n"
		" quote five]\n"
		" \n"
		"# The curly brackets are also processed so the next three\n"
		"# lines will show up as three separate lines\n"
		"{six\n"
		"seven\n"
		"eight}}\n"
		"asserteq_list $l [list one two three 'three;and;a;half' four "
		"five 'six\\nseven\\neight'];"
	},
	{"local",
		"func bits-for {x} {"
			"local y bits;"
			"set y 0 bits 0;"
			"while {$y <= $x} {"
				"inc bits;"
				"set y [expr 1 << $bits]"
			"};"
			"return $bits"
		"};"
		"set y 1001;"
		"set bits [bits-for $y];"
		"set x 45;"
		"set bitsx [bits-for $x];"
		"asserteq 1001 {$y};"
		"asserteq 10 {$bits};"
		"asserteq 45 {$x};"
		"asserteq 6 {$bitsx}"
	},
	{"multiline comment",
		"# this line will not be executed, but the following will\n"
		"set ok1 1\n"
		"## This is a multiline comment\n"
		"   which, as the name implies,\n"
		"   spans multiple lines.\n"
		"set ok2 1\n"
		"   the code above wouldn't execute,\n"
		"   but this will --> ##set ok3 1\n"
		"### more than two #s will not count as multiline comments\n"
		"set ok4 1\n"
		"# Note that semicolons can be used as linebreaks so\n"
		"# this code will be executed: ; set ok5 1\n"
		"##\n"
		"   ...however inside multiline comments semicolons do not\n"
		"   stop the comment section (pretty much like linebreaks)\n"
		"   and this code will not be executed: ; set ok6 1\n"
		"##\n"
		"# Also note that unlike in regular code, semicolons cannot\n"
		"# be escaped in single-line comments, e.g.: ; set ok7 1\n"
		"asserteq_str 1 {$ok1};"
		"assert {![reflect has-var ok2]}"
		"asserteq_str 1 {$ok3};"
		"asserteq_str 1 {$ok4};"
		"asserteq_str 1 {$ok5};"
		"assert {![reflect has-var ok6]}"
		"asserteq_str 1 {$ok7};"
	},
	{"multiline code",
		"asserteq_list [list hello \\\n"
		"	world] [list hello world]"
	},
	{"return",
		"func uses_return {} {"
			"return 1;"
			"return 0;"
		"};"
		"func doesnt_use_return {} {"
			"quote 1;"
		"};"
		"func uses_result {} {"
			"result 1;"
			"quote 0;"
		"};"
		"assert {[uses_return]};"
		"assert {[doesnt_use_return]};"
		"assert {[uses_result]}"
	},
	{"strings",
		"set a 'This is a string';"
		"set b 'This is another string';"
		"asserteq 16 {[length $a]};"
		"asserteq 22 {[length $b]};"
		"asserteq_str a {[charat $a [expr [length $a] / 2]]};"
		"asserteq_str t {[charat $b [expr [length $b] / 2]]};"
		"asserteq 97 {[codeat $a [expr [length $a] / 2]]};"
		"asserteq 116 {[codeat $b [expr [length $b] / 2]]};"
		"asserteq 10 {[strpos $a string]};"
		"asserteq 16 {[strpos $b string]};"
		"asserteq -78 {[strcmp $a $b]};"
		"assert {![streq $a $b]};"
		"asserteq_str 'This is a foo' {[repstr $a string foo]};"
		"asserteq_str 'This is another foo' {[repstr $b string foo]};"
		"asserteq_list [split $a] [list This is a string];"
		"asserteq_list [split $b] [list This is another string];"
	},
	{"topeval",
		"func does-something {} {"
			"topeval {"
				"asserteq 10 {$x};"
				"set x 42;"
				"downeval {set y [expr $x * 10]}"
			"};"
			"asserteq 420 {$y}"
		"};"
		"func calls-something {} {"
			"local x;"
			"set x 33;"
			"does-something;"
			"asserteq 33 {$x};"
			"asserteq 420 {$y}"
		"};"
		"set x 10;"
		"set y 20;"
		"calls-something;"
		"asserteq 42 {$x};"
		"asserteq 420 {$y}"
	},
	{"trim",
		"set str '  Hello,  world! ';"
		"asserteq_str 'Hello,  world!' {[trim $str]};"
		"asserteq_str 'Hello,  world! ' {[ltrim $str]};"
		"asserteq_str '  Hello,  world!' {[rtrim $str]};"
		"asserteq_str 'Hello world' {[foreach [split $str] {"
			"quote [trim $i {,!}]"
		"}]};"
		"asserteq_str 'Hello world' {[filter [split $str {,! }] {"
			"[length $x] > 0"
		"}]};"
	},
};

static int lib_test_lil(struct unit_test_state *uts)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lil_tests); i++) {
		const char *err_msg;
		int err;
		size_t pos;
		struct lil *lil = lil_new(NULL);

		lil_free_value(lil_parse(lil, helpers, sizeof(helpers), 0));
		ut_assert(!lil_error(lil, &err_msg, &pos));
		lil_free_value(lil_parse(lil, lil_tests[i].cmd, 0, 0));
		err = lil_error(lil, &err_msg, &pos);
		if (err) {
			ut_failf(uts, __FILE__, __LINE__, __func__,
				 lil_tests[i].name, "%zu: %s", pos, err_msg);
			lil_free(lil);
			return CMD_RET_FAILURE;
		};
		lil_free(lil);
	}

	return 0;
}
LIB_TEST(lib_test_lil, 0);
