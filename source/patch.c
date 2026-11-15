/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 *
 * MCSM port: the boilerplate's "kuser" patch is DISABLED.
 *
 * That patch scans the game's code for the numbers 0xFFFF0FA0 / 0xFFFF0FC0
 * (addresses of old Linux "kuser" helper functions) and replaces them.
 * libGameEngine.so doesn't use kuser helpers at all (it uses ldrex/strex),
 * but it DOES contain those same numbers as ordinary data: offsets into its
 * global table, inside Meta::Initialize() and elsewhere. Replacing them
 * corrupted those offsets and caused the crash in Meta::Initialize
 * (crash dump: PC 0x986570D8 -> libGameEngine.so + 0x6570D8).
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
#ifdef __cplusplus
};
#endif

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include <stdbool.h>

void so_patch(void) {
	// Nothing to patch yet.
	//
	// Examples for later:
	// hook_addr((uintptr_t)so_symbol(&so_mod, "SomeFunctionName"), (uintptr_t)&my_replacement);
	// hook_addr((uintptr_t)so_mod.text_base + 0x123456, (uintptr_t)&my_replacement);
	l_info("so_patch: no patches applied (kuser patch disabled for MCSM).");
}
