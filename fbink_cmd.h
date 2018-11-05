/*
	FBInk: FrameBuffer eInker, a tool to print text & images on eInk devices (Kobo/Kindle)
	Copyright (C) 2018 NiLuJe <ninuje@gmail.com>

	----

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __FBINK_CMD_H
#define __FBINK_CMD_H

#include "fbink.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

// FBInk always returns negative values on failure
#define ERRCODE(e) (-(e))

static void show_helpmsg(void);

static int do_infinite_progress_bar(int, const FBInkConfig*);

static int strtoul_u(int, const char*, const char*, uint32_t*);
static int strtoul_hu(int, const char*, const char*, unsigned short int*);
static int strtoul_hhu(int, const char*, const char*, uint8_t*);
static int strtol_hi(int, const char*, const char*, short int*);
static int strtol_hhi(int, const char*, const char*, int8_t*);

#endif
