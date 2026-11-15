/*
 * Minecraft: Story Mode - PS Vita loader
 *
 * TEST STAGE 3: "SDL shim"
 * Loads libfmod.so, libfmodstudio.so and libGameEngine.so, links them,
 * then calls the engine's real entry point, Application_SDL::Run().
 *
 * SDL2 is now a small Vita implementation (source/mcsm/imports.c):
 * window + OpenGL context via vitaGL, Vita buttons/sticks as a game
 * controller, front touch screen as touch events.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;

// The engine runs on this (main) thread and needs a big stack.
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod; // libGameEngine.so

extern int g_missing_imports; // from init.c
extern void log_raw(const char *text); // from logger.c

int main() {
    soloader_init_all();
    log_raw("[mcsm] Loader init finished. All libraries initialized.\n");

    // On Android, libmain.so's SDL_main() just jumps straight to
    // Application_SDL::Run() (a static function, no arguments).
    // We skip libmain.so and call it ourselves.
    uintptr_t run = so_symbol(&so_mod, "_ZN15Application_SDL3RunEv");
    if (!run) {
        fatal_error("Engine entry point Application_SDL::Run was not found.");
    }

    int (*Application_SDL_Run)(void) = (void *)run;

    log_raw("[mcsm] Calling Application_SDL::Run()...\n");
    int result = Application_SDL_Run();

    char line[128];
    sceClibSnprintf(line, sizeof(line), "[mcsm] Application_SDL::Run() returned %d\n", result);
    log_raw(line);

    fatal_error("Run test finished!\n\n"
                "Missing imports before stubs: %d\n"
                "Application_SDL::Run returned: %d\n\n"
                "Details are in ux0:data/mcsm/log.txt",
                g_missing_imports, result);
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    // Not used yet
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    // Not used yet
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    // Not used yet
}
