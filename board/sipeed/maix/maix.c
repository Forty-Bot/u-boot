#include <asm/sections.h>
#include <config.h>
#include <fdt_support.h>
#include <hexdump.h>
#include <stdio.h>

void *board_fdt_blob_setup(void)
{
	/* FIXME: the magic number gets overwritten for some reason... */
	u32 *fdt_blob = (u32 *)_end;
	return fdt_blob;
}

int board_init(void)
{
	return 0;
}
