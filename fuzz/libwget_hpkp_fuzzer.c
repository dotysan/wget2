/*
 * Copyright(c) 2017 Free Software Foundation, Inc.
 *
 * This file is part of libwget.
 *
 * Libwget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libwget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libwget.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "wget.h"
#include "fuzzer.h"

static const uint8_t *g_data;
static size_t g_size;

FILE *fopen(const char *pathname, const char *mode)
{
	FILE *(*libc_fopen)(const char *, const char *) =
		(FILE *(*)(const char *, const char *)) dlsym (RTLD_NEXT, "fopen");

	if (!strcmp(pathname, "hpkp"))
		return fmemopen((void *) g_data, g_size, mode);

	return libc_fopen(pathname, mode);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size > 256) // same as max_len = 256 in .options file
		return 0;

	g_data = data;
	g_size = size;

	wget_hpkp_db_t *hpkp_db = wget_hpkp_db_init(NULL);
	wget_hpkp_db_load(hpkp_db, "hpkp");
	wget_hpkp_db_check_pubkey(hpkp_db, "x.y", "0", 1);
	wget_hpkp_db_free(&hpkp_db);

	return 0;
}