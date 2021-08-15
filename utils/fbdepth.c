/*
	FBInk: FrameBuffer eInker, a library to print text & images to an eInk Linux framebuffer
	Copyright (C) 2018-2021 NiLuJe <ninuje@gmail.com>
	SPDX-License-Identifier: GPL-3.0-or-later

	----

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// Because we're pretty much Linux-bound ;).
#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include <errno.h>
#include <getopt.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "../fbink.h"

// Pilfer our usual macros from FBInk...
// We want to return negative values on failure, always
#define ERRCODE(e) (-(e))

// Likely/Unlikely branch tagging
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

bool toSysLog  = false;
bool isQuiet   = false;
bool isVerbose = false;
// Handle what we send to stdout (i.e., mostly diagnostic stuff, which tends to be verbose, so no FBInk tag)
#define LOG(fmt, ...)                                                                                                    \
	({                                                                                                               \
		if (unlikely(isVerbose)) {                                                                               \
			if (toSysLog) {                                                                                  \
				syslog(LOG_INFO, fmt, ##__VA_ARGS__);                                                    \
			} else {                                                                                         \
				fprintf(stdout, fmt "\n", ##__VA_ARGS__);                                                \
			}                                                                                                \
		}                                                                                                        \
	})

// And then what we send to stderr (mostly fbink_init stuff, add an FBInk tag to make it clear where it comes from for API users)
#define ELOG(fmt, ...)                                                                                                   \
	({                                                                                                               \
		if (!isQuiet) {                                                                                          \
			if (toSysLog) {                                                                                  \
				syslog(LOG_NOTICE, "[FBDepth] " fmt, ##__VA_ARGS__);                                     \
			} else {                                                                                         \
				fprintf(stderr, "[FBDepth] " fmt "\n", ##__VA_ARGS__);                                   \
			}                                                                                                \
		}                                                                                                        \
	})

// And a simple wrapper for actual warnings on error codepaths. Should only be used for warnings before a return/exit.
// Always shown, always tagged, and always ends with a bang.
#define WARN(fmt, ...)                                                                                                   \
	({                                                                                                               \
		if (toSysLog) {                                                                                          \
			syslog(LOG_ERR, "[FBDepth] " fmt "!", ##__VA_ARGS__);                                            \
		} else {                                                                                                 \
			fprintf(stderr, "[FBDepth] " fmt "!\n", ##__VA_ARGS__);                                          \
		}                                                                                                        \
	})

// Same, but with __PRETTY_FUNCTION__ right before fmt
#define PFWARN(fmt, ...) ({ WARN("[%s] " fmt, __PRETTY_FUNCTION__, ##__VA_ARGS__); })

// Help message
static void
    show_helpmsg(void)
{
	printf(
	    "\n"
	    "FBDepth (via FBInk %s)\n"
	    "\n"
	    "Usage: fbdepth [-d] <bpp> [-r] <rota>\n"
	    "\n"
	    "Tiny tool to set the framebuffer bitdepth and/or rotation on eInk devices.\n"
	    "\n"
	    "OPTIONS:\n"
	    "\t-d, --depth <8|16|24|32>\t\tSwitch the framebuffer to the supplied bitdepth.\n"
	    "\t-h, --help\t\t\t\tShow this help message.\n"
	    "\t-v, --verbose\t\t\t\tToggle printing diagnostic messages.\n"
	    "\t-q, --quiet\t\t\t\tToggle hiding diagnostic messages.\n"
	    "\t-g, --get\t\t\t\tJust output the current bitdepth to stdout.\n"
	    "\t-G, --getcode\t\t\t\tJust exit with the current bitdepth as exit code.\n"
#if defined(FBINK_FOR_KOBO) || defined(FBINK_FOR_CERVANTES)
	    "\t-r, --rota <-1|0|1|2|3> \t\tSwitch the framebuffer to the supplied rotation. -1 is a magic value matching the device-specific Portrait orientation.\n"
#else
	    "\t-r, --rota <0|1|2|3>\t\tSwitch the framebuffer to the supplied rotation (Linux FB convention).\n"
#endif
#if defined(FBINK_FOR_KOBO)
	    "\t-R, --canonicalrota <UR|CW|UD|CCW>\tSwitch the framebuffer to the supplied canonical rotation (Linux FB convention), automagically translating it to the mangled native one. (i.e., requesting UR will ensure the device is actually UR, much like passing -1 to -r, --rota would).\n"
#endif
	    "\t-o, --getrota\t\t\t\tJust output the current rotation to stdout.\n"
	    "\t-O, --getrotacode\t\t\tJust exit with the current rotation as exit code.\n"
#if defined(FBINK_FOR_KOBO)
	    "\t-c, --getcanonicalrota\t\t\tJust output the current rotation (converted to its canonical representation) to stdout.\n"
	    "\t-C, --getcanonicalrotacode\t\tJust exit with the current rotation (converted to its canonical representation) as exit code.\n"
#endif
	    "\t-H, --nightmode <on|off|toggle>\t\tToggle hardware inversion (8bpp only, safely ignored otherwise).\n"
	    "\n",
	    fbink_version());
	return;
}

// Pilfered from KFMon, with some minor tweaks to make it tri-state...
static int
    strtotristate(const char* restrict str, int8_t* restrict result)
{
	if (!str) {
		WARN("Passed an empty value to a key expecting a tri-state value");
		return ERRCODE(EINVAL);
	}

	switch (str[0]) {
		case 't':
		case 'T':
			if (strcasecmp(str, "true") == 0) {
				*result = true;
				return EXIT_SUCCESS;
			} else if (strcasecmp(str, "toggle") == 0) {
				*result = -1;
				return EXIT_SUCCESS;
			}
			break;
		case 'y':
		case 'Y':
			if (strcasecmp(str, "yes") == 0) {
				*result = true;
				return EXIT_SUCCESS;
			}
			break;
		case '1':
			if (str[1] == '\0') {
				*result = true;
				return EXIT_SUCCESS;
			}
			break;
		case 'f':
		case 'F':
			if (strcasecmp(str, "false") == 0) {
				*result = false;
				return EXIT_SUCCESS;
			}
			break;
		case 'n':
		case 'N':
			switch (str[1]) {
				case 'o':
				case 'O':
					if (str[2] == '\0') {
						*result = false;
						return EXIT_SUCCESS;
					}
					break;
				default:
					break;
			}
			break;
		case '0':
			if (str[1] == '\0') {
				*result = false;
				return EXIT_SUCCESS;
			}
			break;
		case 'o':
		case 'O':
			switch (str[1]) {
				case 'n':
				case 'N':
					if (str[2] == '\0') {
						*result = true;
						return EXIT_SUCCESS;
					}
					break;
				case 'f':
				case 'F':
					switch (str[2]) {
						case 'f':
						case 'F':
							if (str[3] == '\0') {
								*result = false;
								return EXIT_SUCCESS;
							}
							break;
						default:
							break;
					}
					break;
				default:
					break;
			}
			break;
		case '-':
			if (str[1] == '1') {
				if (str[2] == '\0') {
					*result = -1;
					return EXIT_SUCCESS;
				}
			}
			break;
		default:
			// NOTE: *result is initialized to a sentinel value, leave it alone.
			break;
	}

	WARN("Assigned an invalid or malformed value (%s) to a flag expecting a tri-state value", str);
	return ERRCODE(EINVAL);
}

#ifdef FBINK_FOR_KINDLE
static orientation_t
    linuxfb_rotate_to_einkfb_orientation(uint32_t rotate)
{
	switch (rotate) {
		case FB_ROTATE_UR:
			return orientation_portrait;
		case FB_ROTATE_CW:
			return orientation_landscape;
		case FB_ROTATE_UD:
			return orientation_portrait_upside_down;
		case FB_ROTATE_CCW:
			return orientation_landscape_upside_down;
		default:
			// Should never happen.
			return orientation_portrait;
	}
}
#endif

// Duplicated from FBInk itself
static __attribute__((cold)) const char*
    fb_rotate_to_string(uint32_t rotate)
{
	switch (rotate) {
		case FB_ROTATE_UR:
			return "Upright, 0°";
		case FB_ROTATE_CW:
			return "Clockwise, 90°";
		case FB_ROTATE_UD:
			return "Upside Down, 180°";
		case FB_ROTATE_CCW:
			return "Counter Clockwise, 270°";
		default:
			return "Unknown?!";
	}
}

static void
    get_fb_info(int                       fbfd,
		FBInkConfig*              fbink_cfg,
		FBInkState*               fbink_state,
		struct fb_var_screeninfo* var_info,
		struct fb_fix_screeninfo* fix_info)
{
	// We're going to need to current state to check what we actually need to do
	fbink_get_state(fbink_cfg, fbink_state);
	size_t buffer_size = 0U;
	fbink_get_fb_pointer(fbfd, &buffer_size);
	fbink_get_fb_info(var_info, fix_info);

	// Print initial status
	LOG("FBInk state: Screen is %ux%u (%ux%zu), %ubpp @ rotation: %u (%s); buffer size is %zu bytes with a scanline stride of %u bytes",
	    fbink_state->screen_width,
	    fbink_state->screen_height,
	    (fbink_state->scanline_stride << 3U) / fbink_state->bpp,
	    buffer_size / fbink_state->scanline_stride,
	    fbink_state->bpp,
	    fbink_state->current_rota,
	    fb_rotate_to_string(fbink_state->current_rota),
	    buffer_size,
	    fbink_state->scanline_stride);
	LOG("Variable fb info: %ux%u (%ux%u), %ubpp @ rotation: %u (%s)",
	    var_info->xres,
	    var_info->yres,
	    var_info->xres_virtual,
	    var_info->yres_virtual,
	    var_info->bits_per_pixel,
	    var_info->rotate,
	    fb_rotate_to_string(var_info->rotate));
	LOG("Fixed fb info: ID is \"%s\", length of fb mem: %u bytes & line length: %u bytes",
	    fix_info->id,
	    fix_info->smem_len,
	    fix_info->line_length);

#ifdef FBINK_FOR_KINDLE
	// NOTE: einkfb devices (even the K4, which only uses it as a shim over mxcfb HW)
	//       don't actually honor the standard Linux fb rotation, and instead rely on a set of custom ioctls...
	if (fbink_state->is_kindle_legacy) {
		orientation_t orientation = orientation_portrait;
		if (ioctl(fbfd, FBIO_EINK_GET_DISPLAY_ORIENTATION, &orientation)) {
			WARN("FBIO_EINK_GET_DISPLAY_ORIENTATION: %m");
			rv = ERRCODE(EXIT_FAILURE);
			goto cleanup;
		}

		// Because everything is terrible, it's actually not the same mapping as the Linux fb rotate field...
		LOG("Actual einkfb orientation: %u (%s)", orientation, einkfb_orientation_to_string(orientation));
	}
#endif
}

int
    main(int argc, char* argv[])
{
	int                        opt;
	int                        opt_index;
	static const struct option opts[] = { { "depth", required_argument, NULL, 'd' },
					      { "help", no_argument, NULL, 'h' },
					      { "verbose", no_argument, NULL, 'v' },
					      { "quiet", no_argument, NULL, 'q' },
					      { "get", no_argument, NULL, 'g' },
					      { "getcode", no_argument, NULL, 'G' },
					      { "rota", required_argument, NULL, 'r' },
					      { "canonicalrota", required_argument, NULL, 'R' },
					      { "getrota", no_argument, NULL, 'o' },
					      { "getrotacode", no_argument, NULL, 'O' },
					      { "getcanonicalrota", no_argument, NULL, 'c' },
					      { "getcanonicalrotacode", no_argument, NULL, 'C' },
					      { "nightmode", required_argument, NULL, 'H' },
					      { NULL, 0, NULL, 0 } };

	uint32_t req_bpp          = 0U;
	int8_t   req_rota         = 42;
	int8_t   want_nm          = 42;
	bool     errfnd           = false;
	bool     print_bpp        = false;
	bool     return_bpp       = false;
	bool     print_rota       = false;
	bool     return_rota      = false;
	bool     print_canonical  = false;
	bool     return_canonical = false;
	bool     canonical_rota   = false;

	while ((opt = getopt_long(argc, argv, "d:hvqgGr:R:oOcCH:", opts, &opt_index)) != -1) {
		switch (opt) {
			case 'd':
				req_bpp = (uint32_t) strtoul(optarg, NULL, 10);
				// Cheap-ass sanity check
				switch (req_bpp) {
					case 8:
					case 16:
						break;
					case 24:
						// NOTE: Warn that things will probably be wonky...
						//       I'm not quite sure who's to blame: this tool, FBInk, or the Kernel,
						//       but I've never ended up in a useful state on my Kobos.
						//       And I don't have a genuine 24bpp fb device to compare to...
						fprintf(
						    stderr,
						    "Warning! 24bpp handling appears to be broken *somewhere*, you probably don't want to use it!\n\n");
						break;
					case 32:
						break;
					default:
						fprintf(stderr, "Unsupported bitdepth '%s'!\n", optarg);
						errfnd = true;
						break;
				}
				break;
			case 'v':
				isQuiet   = false;
				isVerbose = true;
				break;
			case 'q':
				isQuiet   = true;
				isVerbose = false;
				break;
			case 'h':
				show_helpmsg();
				return EXIT_SUCCESS;
				break;
			case 'g':
				print_bpp = true;
				break;
			case 'G':
				return_bpp = true;
				break;
			case 'r':
				if (strcasecmp(optarg, "UR") == 0 || strcmp(optarg, "0") == 0) {
					req_rota = FB_ROTATE_UR;
				} else if (strcasecmp(optarg, "CW") == 0 || strcmp(optarg, "1") == 0) {
					req_rota = FB_ROTATE_CW;
				} else if (strcasecmp(optarg, "UD") == 0 || strcmp(optarg, "2") == 0) {
					req_rota = FB_ROTATE_UD;
				} else if (strcasecmp(optarg, "CCW") == 0 || strcmp(optarg, "3") == 0) {
					req_rota = FB_ROTATE_CCW;
				} else if (strcmp(optarg, "-1") == 0) {
					req_rota = -1;
				} else {
					fprintf(stderr, "Invalid rotation '%s'!\n", optarg);
					errfnd = true;
				}
				break;
			case 'R':
#if defined(FBINK_FOR_KOBO)
				if (strcasecmp(optarg, "UR") == 0 || strcmp(optarg, "0") == 0) {
					req_rota = FB_ROTATE_UR;
				} else if (strcasecmp(optarg, "CW") == 0 || strcmp(optarg, "1") == 0) {
					req_rota = FB_ROTATE_CW;
				} else if (strcasecmp(optarg, "UD") == 0 || strcmp(optarg, "2") == 0) {
					req_rota = FB_ROTATE_UD;
				} else if (strcasecmp(optarg, "CCW") == 0 || strcmp(optarg, "3") == 0) {
					req_rota = FB_ROTATE_CCW;
				} else {
					fprintf(stderr, "Invalid rotation '%s'!\n", optarg);
					errfnd = true;
				}
				canonical_rota = true;
#else
				fprintf(stderr, "This option (-R, --canonicalrota) is not supported on your device!\n");
				errfnd = true;
#endif
				break;
			case 'o':
				print_rota = true;
				break;
			case 'O':
				return_rota = true;
				break;
			case 'c':
#if defined(FBINK_FOR_KOBO)
				print_canonical = true;
#else
				fprintf(stderr,
					"This option (-c, --getcanonicalrota) is not supported on your device!\n");
				errfnd = true;
#endif
				break;
			case 'C':
#if defined(FBINK_FOR_KOBO)
				return_canonical = true;
#else
				fprintf(stderr,
					"This option (-C, --getcanonicalrotacode) is not supported on your device!\n");
				errfnd = true;
#endif
				break;
			case 'H':
				if (strtotristate(optarg, &want_nm) < 0) {
					fprintf(stderr, "Invalid nightmode state '%s'!\n", optarg);
					errfnd = true;
				}
				break;
			default:
				fprintf(stderr, "?? Unknown option code 0%o ??\n", (unsigned int) opt);
				errfnd = true;
				break;
		}
	}

	if (errfnd || ((req_bpp == 0U && req_rota == 42 && want_nm == 42) &&
		       !(print_bpp || return_bpp || print_rota || return_rota || print_canonical || return_canonical))) {
		show_helpmsg();
		return ERRCODE(EXIT_FAILURE);
	}

	// Enforce quiet w/ print_*
	if (print_bpp || print_rota || print_canonical) {
		isQuiet   = true;
		isVerbose = false;
	}

	// Assume success, until shit happens ;)
	int rv = EXIT_SUCCESS;

	// Init FBInk
	FBInkConfig fbink_cfg = { 0 };
	fbink_cfg.is_verbose  = isVerbose;
	fbink_cfg.is_quiet    = isQuiet;
	int fbfd              = -1;
	// Open framebuffer and keep it around, then setup globals.
	if ((fbfd = fbink_open()) == ERRCODE(EXIT_FAILURE)) {
		fprintf(stderr, "Failed to open the framebuffer, aborting . . .\n");
		rv = ERRCODE(EXIT_FAILURE);
		goto cleanup;
	}
	if (fbink_init(fbfd, &fbink_cfg) != EXIT_SUCCESS) {
		fprintf(stderr, "Failed to initialize FBInk, aborting . . .\n");
		rv = ERRCODE(EXIT_FAILURE);
		goto cleanup;
	}

	// We're also going to need to current state to check what we actually need to do
	FBInkState               fbink_state = { 0 };
	struct fb_var_screeninfo var_info    = { 0 };
	struct fb_fix_screeninfo fix_info    = { 0 };
	// Print initial status
	get_fb_info(fbfd, &fbink_cfg, &fbink_state, &var_info, &fix_info);

	// If we just wanted to print/return the current bitdepth, abort early
	if (print_bpp || return_bpp) {
		if (print_bpp) {
			fprintf(stdout, "%u", fbink_state.bpp);
		}
		if (return_bpp) {
			rv = (int) fbink_state.bpp;
			goto cleanup;
		} else {
			goto cleanup;
		}
	}

	// If we just wanted to print/return the current rotation, abort early
	if (print_rota || return_rota) {
		if (print_rota) {
			fprintf(stdout, "%u", fbink_state.current_rota);
		}
		if (return_rota) {
			rv = (int) fbink_state.current_rota;
			goto cleanup;
		} else {
			goto cleanup;
		}
	}

#if defined(FBINK_FOR_KOBO)
	// If we just wanted to print/return the current canonical rotation, abort early
	if (print_canonical || return_canonical) {
		if (print_canonical) {
			fprintf(stdout, "%hhu", fbink_rota_native_to_canonical(fbink_state.current_rota));
		}
		if (return_canonical) {
			rv = (int) fbink_rota_native_to_canonical(fbink_state.current_rota);
			goto cleanup;
		} else {
			goto cleanup;
		}
	}
#endif

	// If no bitdepth was requested, set to the current one, we'll be double-checking if changes are actually needed.
	if (req_bpp == 0U) {
		req_bpp = fbink_state.bpp;
	}

	// If the automagic Portrait rotation was requested, compute it
#if defined(FBINK_FOR_KOBO) || defined(FBINK_FOR_CERVANTES)
	if (req_rota == -1) {
		// NOTE: For *most* devices, Nickel's Portrait orientation should *always* match BootRota + 1
		//       Thankfully, the Libra appears to be ushering in a new era filled with puppies and rainbows,
		//       and, hopefully, less insane rotation quirks ;).
		if (fbink_state.ntx_rota_quirk != NTX_ROTA_SANE) {
			req_rota = (fbink_state.ntx_boot_rota + 1) & 3;
		} else {
			req_rota = (int8_t) fbink_state.ntx_boot_rota;
		}
		LOG("Device's expected Portrait orientation should be: %hhd (%s)!",
		    req_rota,
		    fb_rotate_to_string((uint32_t) req_rota));
	}
#endif

	// If no rotation was requested, reset req_rota to our expected sentinel value
	if (req_rota == 42) {
		req_rota = -1;
	}

	// Ensure the requested rotation is sane (if all is well, this should never be tripped)
	if (req_rota < -1 || req_rota > FB_ROTATE_CCW) {
		LOG("Requested rotation (%hhd) is bogus, discarding it!\n", req_rota);
		req_rota = -1;
	}

	// Compute the proper grayscale flag given the current bitdepth and whether we want to enable nightmode or not...
	// We rely on the EPDC feature that *toggles* HW inversion, no matter the bitdepth
	// (by essentially *flipping* the EPDC_FLAG_ENABLE_INVERSION flag, on the kernel side).
	// c.f., epdc_process_update @ mxc_epdc_fb
	// In practice, though, grayscale != 0U is *invalid* for > 8bpp (and it does break rendering),
	// so we only play with this @ 8bpp ;).
	// NOTE: While we technically don't allow switching to 4bpp, make sure we leave it alone,
	//       because there are dedicated GRAYSCALE_4BIT & GRAYSCALE_4BIT_INVERTED constants...
	uint32_t req_gray = KEEP_CURRENT_GRAYSCALE;
	if (want_nm == true) {
		if (req_bpp == 8U) {
			req_gray = 2U;    // GRAYSCALE_8BIT_INVERTED
		} else if (req_bpp > 8U) {
			req_gray = 0U;
		}
	} else if (want_nm == false) {
		if (req_bpp == 8U) {
			req_gray = 1U;    // GRAYSCALE_8BIT
		} else if (req_bpp > 8U) {
			req_gray = 0U;
		}
	} else if (want_nm == -1) {
		req_gray = TOGGLE_GRAYSCALE;
	} else {
		// Otherwise, make sure we default to sane values for a non-inverted palette...
		if (req_bpp == 8U) {
			req_gray = 1U;    // GRAYSCALE_8BIT
		} else if (req_bpp > 8U) {
			req_gray = 0U;
		}
	}

	// If a change was requested, do it, but check if it's necessary first
	bool is_change_needed = false;

	// Start by checking that the grayscale flag is flipped properly
	if (var_info.grayscale == req_gray) {
		LOG("\nCurrent grayscale flag is already %u!", req_gray);
		// No change needed as far as grayscale is concerned...
	} else {
		is_change_needed = true;
	}

	// Then bitdepth...
	if (fbink_state.bpp == req_bpp) {
		// Also check that the grayscale flag is flipped properly (again)
		if (var_info.grayscale != req_gray) {
			LOG("\nCurrent bitdepth is already %ubpp, but the grayscale flag is bogus!", req_bpp);
			// Continue, we'll need to flip the grayscale flag properly
			is_change_needed = true;
		} else {
			LOG("\nCurrent bitdepth is already %ubpp!", req_bpp);
			// No change needed as far as bitdepth is concerned...
		}
	} else {
		is_change_needed = true;
	}

	// Same for rotation, if we requested one...
	if (req_rota != -1) {
#ifdef FBINK_FOR_KINDLE
		if (fbink_state.is_kindle_legacy) {
			// We need to check the effective orientation on einkfb...
			orientation_t orientation = orientation_portrait;
			if (ioctl(fbfd, FBIO_EINK_GET_DISPLAY_ORIENTATION, &orientation)) {
				WARN("FBIO_EINK_GET_DISPLAY_ORIENTATION: %m");
				rv = ERRCODE(EXIT_FAILURE);
				goto cleanup;
			}

			uint32_t rotate = einkfb_orientation_to_linuxfb_rotate(orientation);
			if (rotate == (uint32_t) req_rota) {
				LOG("\nCurrent rotation is already %hhd!", req_rota);
				// No change needed as far as rotation is concerned...
			} else {
				is_change_needed = true;
			}
		} else {
			// On mxcfb, everything's peachy
			if (vInfo.rotate == (uint32_t) req_rota) {
				LOG("\nCurrent rotation is already %hhd!", req_rota);
				// No change needed as far as rotation is concerned...
			} else {
				is_change_needed = true;
			}
		}
#else
#	if defined(FBINK_FOR_KOBO)
		// If the requested rota was canonical, translate it to a native one *now*
		if (canonical_rota) {
			LOG("\nRequested canonical rota %hhd translates to %u for this device",
			    req_rota,
			    fbink_rota_canonical_to_native((uint8_t) req_rota));
			req_rota = (int8_t) fbink_rota_canonical_to_native((uint8_t) req_rota);
		}
#	endif
		if (fbink_state.current_rota == (uint32_t) req_rota) {
			LOG("\nCurrent rotation is already %hhd!", req_rota);
			// No change needed as far as rotation is concerned...
		} else {
			is_change_needed = true;
		}
#endif
	}

	// If it turns out that no actual changes are needed, skip to cleanup, exiting successfully
	if (!is_change_needed) {
		goto cleanup;
	}

	// If we're here, we really want to change the bitdepth and/or rota ;)
	if (req_rota != -1) {
		LOG("\nSwitching fb to %ubpp%s @ rotation %hhd . . .",
		    req_bpp,
		    (req_bpp == fbink_state.bpp) ? " (current bitdepth)" : "",
		    req_rota);
	} else {
		LOG("\nSwitching fb to %ubpp%s . . .",
		    req_bpp,
		    (req_bpp == fbink_state.bpp) ? " (current bitdepth)" : "");
	}
	if (fbink_set_fb_info(fbfd, req_rota, !canonical_rota, req_bpp, req_gray, &fbink_cfg) < 0) {
		rv = ERRCODE(EXIT_FAILURE);
		goto cleanup;
	}
	// Recap
	get_fb_info(fbfd, &fbink_cfg, &fbink_state, &var_info, &fix_info);

cleanup:
	if (fbink_close(fbfd) == ERRCODE(EXIT_FAILURE)) {
		WARN("Failed to close the framebuffer, aborting");
		rv = ERRCODE(EXIT_FAILURE);
	}

	return rv;
}
