/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * MCSM port changes:
 *  - Loads three Android libraries instead of one:
 *      libfmod.so -> libfmodstudio.so -> libGameEngine.so
 *    (the engine needs both FMOD libraries for audio).
 *  - Writes a report of every import that nothing on the Vita provides yet
 *    to the log (ux0:data/mcsm/log.txt), so we know what to implement next.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <reimpl/controls.h>

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <vitaGL.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Where each Android library is placed in memory.
// They need separate, non-overlapping address ranges, and must stay out of
// the area used by the 256 MB heap (that's why 0x90000000 failed).
// Each library also gets a 64 KB patch area just below its address.
//   libGameEngine.so : 0x98000000 - ~0x990A6000 (about 17 MB)
//   libfmod.so       : 0x99200000 - ~0x9935C000 (about 1.4 MB)
//   libfmodstudio.so : 0x99600000 - ~0x99772000 (about 1.5 MB)
#define LOAD_ADDRESS             0x98000000 // libGameEngine.so
#define FMOD_LOAD_ADDRESS        0x99200000
#define FMODSTUDIO_LOAD_ADDRESS  0x99600000

#define FMOD_SO_PATH        DATA_PATH "libfmod.so"
#define FMODSTUDIO_SO_PATH  DATA_PATH "libfmodstudio.so"

extern so_module so_mod;      // libGameEngine.so (declared in main.c)
so_module fmod_mod;           // libfmod.so
so_module fmodstudio_mod;     // libfmodstudio.so

// From dynlib.c
extern int dynlib_has_symbol(const char *symbol);
// From logger.c
extern void log_raw(const char *text);

static void check_file(const char *path) {
    if (!file_exists(path)) {
        fatal_error("Missing game file:\n%s\n\nCopy the files from the APK's "
                    "lib/armeabi-v7a folder into ux0:data/mcsm/.", path);
    }
}

static void load_module(so_module *mod, const char *path, uintptr_t addr, const char *name) {
    check_file(path);
    int res = so_file_load(mod, path, addr);
    if (res < 0) {
        l_fatal("%s could not be loaded (error 0x%08X).", name, (unsigned int)res);
        fatal_error("Error: could not load %s.\n\nError code: 0x%08X\nLoad address: 0x%08X",
                    path, (unsigned int)res, (unsigned int)addr);
    }
    l_success("%s loaded at 0x%08X.", name, (unsigned int)addr);
}

// Is a symbol exported by one of our three loaded libraries?
static int exported_by_loaded_libs(const char *symbol) {
    return so_symbol(&fmod_mod, symbol) ||
           so_symbol(&fmodstudio_mod, symbol) ||
           so_symbol(&so_mod, symbol);
}

// Points one import of `mod` (its GOT slot) at `fn`, even when the symbol
// was already linked to another loaded library. Returns slots changed.
static int redirect_import(so_module *mod, const char *symbol, uintptr_t fn) {
    int changed = 0;
    for (uint32_t i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i]
                                             : &mod->relplt[i - mod->num_reldyn];
        int type = ELF32_R_TYPE(rel->r_info);
        if (type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
            continue;
        Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
        if (strcmp(mod->dynstr + sym->st_name, symbol) == 0) {
            *(uintptr_t *)(mod->load_addr + rel->r_offset) = fn;
            changed++;
        }
    }
    return changed;
}

/* ---------------------------------------------------------------------- */
/* Audio: force FMOD's "no sound" output for now                           */
/* FMOD on Android outputs through OpenSL ES or Java AudioTrack, neither of */
/* which exists here yet, and its failed output init crashed the audio      */
/* thread. With NOSOUND, FMOD runs normally but plays nothing, so the game  */
/* can boot. Real audio (a Vita output for FMOD) comes later.               */
/* ---------------------------------------------------------------------- */

#define FMOD_OUTPUTTYPE_NOSOUND 2

typedef int (*studio_initialize_fn)(void *self, int maxchannels, unsigned studioflags,
                                    unsigned flags, void *extradriverdata);
typedef int (*get_lowlevel_fn)(const void *self, void **system);
typedef int (*set_output_fn)(void *system, int output);

static studio_initialize_fn real_studio_initialize;

static int studio_initialize_nosound(void *self, int maxchannels, unsigned studioflags,
                                     unsigned flags, void *extradriverdata) {
    get_lowlevel_fn get_ll = (get_lowlevel_fn) so_symbol(&fmodstudio_mod,
        "_ZNK4FMOD6Studio6System17getLowLevelSystemEPPNS_6SystemE");
    set_output_fn set_output = (set_output_fn) so_symbol(&fmod_mod,
        "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
    void *ll = NULL;
    int r1 = get_ll ? get_ll(self, &ll) : -1;
    int r2 = (ll && set_output) ? set_output(ll, FMOD_OUTPUTTYPE_NOSOUND) : -1;
    l_info("FMOD: forcing NOSOUND output (getLowLevelSystem %d, setOutput %d)", r1, r2);

    int res = real_studio_initialize(self, maxchannels, studioflags, flags, extradriverdata);
    l_info("FMOD::Studio::System::initialize(%d, 0x%x, 0x%x) -> %d",
           maxchannels, studioflags, flags, res);
    return res;
}

// Writes every import of `mod` that nothing can provide yet to the log.
// Returns how many were missing.
static int report_missing_imports(const so_module *mod, const char *name) {
    char line[512];
    int missing = 0;

    sceClibSnprintf(line, sizeof(line), "\n===== Missing imports for %s =====\n", name);
    log_raw(line);

    for (uint32_t i = 1; i < mod->num_dynsym; i++) {
        const Elf32_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF || sym->st_name == 0)
            continue;
        if (ELF32_ST_BIND(sym->st_info) == STB_WEAK)
            continue; // weak imports are optional

        const char *symbol = mod->dynstr + sym->st_name;

        if (dynlib_has_symbol(symbol))      continue; // provided by dynlib.c
        if (exported_by_loaded_libs(symbol)) continue; // provided by another .so
        if (vglGetProcAddress(symbol))      continue; // OpenGL function from vitaGL

        sceClibSnprintf(line, sizeof(line), "%s\n", symbol);
        log_raw(line);
        missing++;
    }

    sceClibSnprintf(line, sizeof(line), "===== %d missing in %s =====\n", missing, name);
    log_raw(line);
    return missing;
}

int g_missing_imports = 0;

void soloader_init_all() {
    // Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    l_info("MCSM Vita loader starting.");

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    // 1. Load all three libraries into memory (order matters:
    //    FMOD first, then FMOD Studio which uses it, then the engine).
    load_module(&fmod_mod, FMOD_SO_PATH, FMOD_LOAD_ADDRESS, "libfmod.so");
    load_module(&fmodstudio_mod, FMODSTUDIO_SO_PATH, FMODSTUDIO_LOAD_ADDRESS, "libfmodstudio.so");
    load_module(&so_mod, SO_PATH, LOAD_ADDRESS, "libGameEngine.so");

    settings_load();
    l_success("Settings loaded.");

    // 2. Report what is still missing (written to log.txt)
    g_missing_imports  = report_missing_imports(&fmod_mod, "libfmod.so");
    g_missing_imports += report_missing_imports(&fmodstudio_mod, "libfmodstudio.so");
    g_missing_imports += report_missing_imports(&so_mod, "libGameEngine.so");
    l_info("Total missing imports: %d", g_missing_imports);

    // 3. Relocate and link each library
    so_relocate(&fmod_mod);
    so_relocate(&fmodstudio_mod);
    so_relocate(&so_mod);
    l_success("All libraries relocated.");

    resolve_imports(&fmod_mod);
    resolve_imports(&fmodstudio_mod);
    resolve_imports(&so_mod);
    l_success("All imports resolved.");

    real_studio_initialize = (studio_initialize_fn) so_symbol(&fmodstudio_mod,
        "_ZN4FMOD6Studio6System10initializeEijjPv");
    if (real_studio_initialize &&
        redirect_import(&so_mod, "_ZN4FMOD6Studio6System10initializeEijjPv",
                        (uintptr_t)&studio_initialize_nosound) > 0)
        l_success("FMOD Studio initialize hooked (NOSOUND).");
    else
        l_error("Could not hook FMOD::Studio::System::initialize!");

    // 4. Game-specific patches (patch.c)
    so_patch();
    l_success("SO patched.");

    so_flush_caches(&fmod_mod);
    so_flush_caches(&fmodstudio_mod);
    so_flush_caches(&so_mod);
    l_success("Caches flushed.");

    // 5. Run each library's startup code, dependencies first
    so_initialize(&fmod_mod);
    so_initialize(&fmodstudio_mod);
    so_initialize(&so_mod);
    l_success("All libraries initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

    // Android calls JNI_OnLoad when System.loadLibrary("fmod") runs. FMOD
    // saves the JavaVM there; without it, FMOD::Studio::System::initialize
    // crashes calling GetEnv on a NULL JavaVM (audio thread, libfmod+0xcd978).
    int (*fmod_JNI_OnLoad)(JavaVM *, void *) =
        (int (*)(JavaVM *, void *)) so_symbol(&fmod_mod, "JNI_OnLoad");
    if (fmod_JNI_OnLoad) {
        int ver = fmod_JNI_OnLoad(&jvm, NULL);
        l_success("libfmod.so JNI_OnLoad -> 0x%x", ver);
    } else {
        l_error("libfmod.so has no JNI_OnLoad!");
    }

#ifndef NDK_PORT
    controls_init();
    l_success("Controls initialized.");
#endif
}
