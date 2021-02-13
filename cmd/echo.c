// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2000-2009
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <common.h>
#include <command.h>
#include <malloc.h>

DECLARE_GLOBAL_DATA_PTR;

static int do_echo(struct cmd_tbl *cmdtp, int flag, int argc,
		   char *const argv[])
{
	char *result;
	int j, i = 1;
	size_t result_size, arglens[CONFIG_SYS_MAXARGS];
	bool space = false;
	bool newline = true;

	if (argc > 1) {
		if (!strcmp(argv[1], "-n")) {
			newline = false;
			++i;
		}
	}

	result_size = 1 + newline; /* \0 + \n */
	result_size += argc - i - 1; /* spaces */
	for (j = i; j < argc; ++j) {
		arglens[j] = strlen(argv[j]);
		result_size += arglens[j];
	}

	result = malloc(result_size);
	if (!result)
		return CMD_RET_FAILURE;
	gd->cmd_result = result;

	for (; i < argc; ++i) {
		if (space)
			*result++ = ' ';

		memcpy(result, argv[i], arglens[i]);
		result += arglens[i];
		space = true;
	}

	if (newline)
		*result++ = '\n';
	*result = '\0';

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	echo, CONFIG_SYS_MAXARGS, 1, do_echo,
	"echo args to console",
	"[-n] [args..]\n"
	"    - echo args to console; -n suppresses newline"
);
