/*
 * Minecraft: Story Mode - PS Vita loader
 *
 * imports.c - Extra symbols the game libraries need that the boilerplate's
 * dynlib.c doesn't provide.
 *
 *  1. A small SDL2 replacement ("SDL shim"). The engine only uses ~45 SDL
 *     functions, so instead of porting the whole SDL2 library we implement
 *     just those, on top of vitaGL (graphics), sceCtrl (buttons) and
 *     sceTouch (touch screen).
 *  2. SDL's Android helpers (normally inside the Android libSDL2.so).
 *  3. Simple implementations of small libc/POSIX functions.
 *
 * Everything the engine does through SDL is logged to log.txt
 * (the first few calls of each function only).
 */

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/types.h>

#include <vitaGL.h>

#include <so_util/so_util.h>
#include <falso_jni/FalsoJNI.h>

#include "utils/logger.h"
#include "utils/glutil.h"

extern void log_raw(const char *text);

/* ====================================================================== */
/* Logging helpers                                                        */
/* ====================================================================== */

#define LOG_LIMIT 3 // log only the first 3 calls of each function

static void logf_line(const char *fmt, ...) {
    char line[512];
    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(line, sizeof(line), fmt, list);
    va_end(list);
    log_raw(line);
}

// Logs "[sdl] <name> ..." only for the first LOG_LIMIT calls.
#define LOG_FIRST(fmt, ...) do { \
        static int _c = 0; \
        if (_c < LOG_LIMIT) logf_line("[sdl] " fmt "%s\n", ##__VA_ARGS__, \
                                      (_c == LOG_LIMIT - 1) ? " (further calls not logged)" : ""); \
        _c++; \
    } while (0)

/* ====================================================================== */
/* SDL2 types (only what we need, same memory layout as real SDL2)        */
/* ====================================================================== */

typedef struct { int x, y, w, h; } SDL_Rect;

// SDL_Event is a 56-byte union. We only fill the event kinds we generate.
typedef union {
    uint32_t type;
    uint8_t padding[56];
} SDL_Event;

typedef struct { // SDL_ControllerDeviceEvent / SDL_JoyDeviceEvent
    uint32_t type;
    uint32_t timestamp;
    int32_t which;
} SDL_DeviceEvent;

typedef struct { // SDL_ControllerButtonEvent
    uint32_t type;
    uint32_t timestamp;
    int32_t which;
    uint8_t button;
    uint8_t state;
    uint8_t padding1, padding2;
} SDL_ControllerButtonEvent;

typedef struct { // SDL_ControllerAxisEvent
    uint32_t type;
    uint32_t timestamp;
    int32_t which;
    uint8_t axis;
    uint8_t padding1, padding2, padding3;
    int16_t value;
    uint16_t padding4;
} SDL_ControllerAxisEvent;

typedef struct { // SDL_TouchFingerEvent
    uint32_t type;
    uint32_t timestamp;
    int64_t touchId;
    int64_t fingerId;
    float x, y, dx, dy, pressure;
} SDL_TouchFingerEvent;

// Event type numbers (from SDL_events.h)
#define SDL_QUIT                    0x100
#define SDL_JOYDEVICEADDED          0x605
#define SDL_CONTROLLERAXISMOTION    0x650
#define SDL_CONTROLLERBUTTONDOWN    0x651
#define SDL_CONTROLLERBUTTONUP      0x652
#define SDL_CONTROLLERDEVICEADDED   0x653
#define SDL_FINGERDOWN              0x700
#define SDL_FINGERUP                0x701
#define SDL_FINGERMOTION            0x702

// SDL game controller buttons (SDL_GameControllerButton)
enum {
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_BACK, BTN_GUIDE, BTN_START,
    BTN_LEFTSTICK, BTN_RIGHTSTICK, BTN_LEFTSHOULDER, BTN_RIGHTSHOULDER,
    BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
    BTN_COUNT
};

// SDL game controller axes (SDL_GameControllerAxis)
enum { AXIS_LEFTX, AXIS_LEFTY, AXIS_RIGHTX, AXIS_RIGHTY,
       AXIS_TRIGGERLEFT, AXIS_TRIGGERRIGHT, AXIS_COUNT };

/* ====================================================================== */
/* Event queue                                                            */
/* ====================================================================== */

#define QUEUE_SIZE 128
static SDL_Event event_queue[QUEUE_SIZE];
static int queue_head = 0, queue_tail = 0;

static void queue_push(const void *ev, size_t size) {
    int next = (queue_tail + 1) % QUEUE_SIZE;
    if (next == queue_head) return; // full: drop
    sceClibMemset(&event_queue[queue_tail], 0, sizeof(SDL_Event));
    sceClibMemcpy(&event_queue[queue_tail], ev, size);
    queue_tail = next;
}

static int queue_pop(SDL_Event *out) {
    if (queue_head == queue_tail) return 0;
    if (out) *out = event_queue[queue_head];
    queue_head = (queue_head + 1) % QUEUE_SIZE;
    return 1;
}

static uint32_t ticks_ms(void) {
    return (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
}

/* ====================================================================== */
/* Input (Vita buttons, sticks, touch -> SDL controller + touch)          */
/* ====================================================================== */

static uint8_t  btn_state[BTN_COUNT];
static int16_t  axis_state[AXIS_COUNT];
static int      input_needs_poll = 1;
static int      devices_announced = 0;

// Vita button -> SDL controller button
static const struct { uint32_t vita; int sdl; } button_map[] = {
    { SCE_CTRL_CROSS,    BTN_A },
    { SCE_CTRL_CIRCLE,   BTN_B },
    { SCE_CTRL_SQUARE,   BTN_X },
    { SCE_CTRL_TRIANGLE, BTN_Y },
    { SCE_CTRL_SELECT,   BTN_BACK },
    { SCE_CTRL_START,    BTN_START },
    { SCE_CTRL_L1,       BTN_LEFTSHOULDER },
    { SCE_CTRL_R1,       BTN_RIGHTSHOULDER },
    { SCE_CTRL_L3,       BTN_LEFTSTICK },  // PS TV only
    { SCE_CTRL_R3,       BTN_RIGHTSTICK }, // PS TV only
    { SCE_CTRL_UP,       BTN_DPAD_UP },
    { SCE_CTRL_DOWN,     BTN_DPAD_DOWN },
    { SCE_CTRL_LEFT,     BTN_DPAD_LEFT },
    { SCE_CTRL_RIGHT,    BTN_DPAD_RIGHT },
};

static int16_t stick_to_axis(uint8_t v) {
    int centered = (int)v - 128;          // -128..127
    if (abs(centered) < 16) centered = 0; // deadzone
    int value = centered * 256;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

static void poll_buttons_and_sticks(void) {
    SceCtrlData pad;
    sceClibMemset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

    for (size_t i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
        int sdl = button_map[i].sdl;
        uint8_t pressed = (pad.buttons & button_map[i].vita) ? 1 : 0;
        if (pressed != btn_state[sdl]) {
            btn_state[sdl] = pressed;
            SDL_ControllerButtonEvent ev = {0};
            ev.type = pressed ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
            ev.timestamp = ticks_ms();
            ev.which = 0;
            ev.button = (uint8_t)sdl;
            ev.state = pressed;
            queue_push(&ev, sizeof(ev));
        }
    }

    int16_t new_axes[AXIS_COUNT] = {
        stick_to_axis(pad.lx), stick_to_axis(pad.ly),
        stick_to_axis(pad.rx), stick_to_axis(pad.ry),
        (pad.buttons & SCE_CTRL_L2) ? 32767 : 0, // PS TV only
        (pad.buttons & SCE_CTRL_R2) ? 32767 : 0, // PS TV only
    };
    for (int a = 0; a < AXIS_COUNT; a++) {
        if (new_axes[a] != axis_state[a]) {
            axis_state[a] = new_axes[a];
            SDL_ControllerAxisEvent ev = {0};
            ev.type = SDL_CONTROLLERAXISMOTION;
            ev.timestamp = ticks_ms();
            ev.which = 0;
            ev.axis = (uint8_t)a;
            ev.value = new_axes[a];
            queue_push(&ev, sizeof(ev));
        }
    }
}

// Front touch panel is 1920x1088 in touch units
#define TOUCH_W 1920.0f
#define TOUCH_H 1088.0f
#define MAX_FINGERS 6

static struct { int active; int id; float x, y; } fingers[MAX_FINGERS];

static void poll_touch(void) {
    SceTouchData touch;
    sceClibMemset(&touch, 0, sizeof(touch));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

    int seen[MAX_FINGERS] = {0};

    for (int r = 0; r < (int)touch.reportNum && r < MAX_FINGERS; r++) {
        int id = touch.report[r].id;
        float x = touch.report[r].x / TOUCH_W;
        float y = touch.report[r].y / TOUCH_H;

        int slot = -1;
        for (int f = 0; f < MAX_FINGERS; f++)
            if (fingers[f].active && fingers[f].id == id) { slot = f; break; }

        SDL_TouchFingerEvent ev = {0};
        ev.timestamp = ticks_ms();
        ev.touchId = 1;
        ev.fingerId = id;
        ev.x = x; ev.y = y;
        ev.pressure = 1.0f;

        if (slot < 0) { // new finger
            for (int f = 0; f < MAX_FINGERS; f++)
                if (!fingers[f].active) { slot = f; break; }
            if (slot < 0) continue;
            fingers[slot].active = 1;
            fingers[slot].id = id;
            ev.type = SDL_FINGERDOWN;
        } else {        // moving finger
            if (x == fingers[slot].x && y == fingers[slot].y) { seen[slot] = 1; continue; }
            ev.type = SDL_FINGERMOTION;
            ev.dx = x - fingers[slot].x;
            ev.dy = y - fingers[slot].y;
        }
        fingers[slot].x = x;
        fingers[slot].y = y;
        seen[slot] = 1;
        queue_push(&ev, sizeof(ev));
    }

    for (int f = 0; f < MAX_FINGERS; f++) { // released fingers
        if (fingers[f].active && !seen[f]) {
            SDL_TouchFingerEvent ev = {0};
            ev.type = SDL_FINGERUP;
            ev.timestamp = ticks_ms();
            ev.touchId = 1;
            ev.fingerId = fingers[f].id;
            ev.x = fingers[f].x; ev.y = fingers[f].y;
            queue_push(&ev, sizeof(ev));
            fingers[f].active = 0;
        }
    }
}

/* ====================================================================== */
/* SDL functions                                                          */
/* ====================================================================== */

static int  sdl_gl_ready = 0;
static int  fake_window = 0x57494E44; // just needs a unique address
static int  fake_controller = 0x43545231;

/* ---------------------------------------------------------------------- */
/* Watchdog: if the game hasn't presented a frame after WATCHDOG_SECONDS,  */
/* deliberately crash this helper thread. The Vita then writes a           */
/* psp2core dump that shows where EVERY thread (incl. the stuck main       */
/* thread) was at that moment. Debug aid; remove once the game renders.    */
/* ---------------------------------------------------------------------- */

#define WATCHDOG_SECONDS 60

static volatile int frames_presented = 0;
static volatile uint32_t poll_calls, gl_clears, gl_draws, gl_fbo_binds;
static volatile SceUID gl_last_thread;
static SceUID main_thread_id;
static volatile uint32_t pread_calls;
static volatile uint64_t pread_bytes;

static int watchdog_thread(SceSize args, void *argp) {
    for (int s = 0; s < WATCHDOG_SECONDS; s++) {
        sceKernelDelayThread(1000 * 1000);
        if (frames_presented > 0) {
            logf_line("[watchdog] first frame after ~%d s, watchdog off\n", s + 1);
            return 0;
        }
        if ((s + 1) % 10 == 0) {
            // Engine globals (libGameEngine.so @ 0x98000000, offsets from its symbols)
            #define ENG(off) ((volatile uint8_t *)(0x98000000 + (off)))
            uint8_t  mt_enable  = *ENG(0x1031880);                    // gMultithreadRenderEnable
            uint8_t  suspended  = *ENG(0x1098518);                    // GameEngine::mbSuspendGameLoop
            uint32_t frame_num  = *(volatile uint32_t *)ENG(0x1099350); // Metrics::mFrameNum
            float    frame_time = *(volatile float *)ENG(0x1099538);    // Metrics::mFrameTime
            float    actual_ft  = *(volatile float *)ENG(0x109b898);    // Metrics::mActualFrameTime
            uint32_t ctx        = *(volatile uint32_t *)ENG(0x106b7a0); // render thread context
            uint32_t rframe_idx = ctx ? *(volatile uint32_t *)(ctx + 0x20c) : 0; // RenderThread frame index
            // GameEngine::RenderFX gates (all must allow rendering)
            uint8_t  reinit     = *ENG(0x1098508);                     // RenderDevice::mbReInitializeDevice (must be 0)
            uint8_t  enable_r   = *ENG(0x1031efe);                     // RenderDevice::mbEnableRendering (must be 1)
            int32_t  delay_fr   = *(volatile int32_t *)ENG(0x1098370);  // RenderDevice::mRenderDelayFrames
            uint32_t game_win   = *(volatile uint32_t *)ENG(0x106a4c0); // GameWindow::smpGameWin (must be != 0)
            // Fast sampling: max frame times seen over ~200 ms (single snapshots
            // almost always land on a 0 ms loop iteration).
            float max_ft = 0.0f, max_aft = 0.0f;
            for (int k = 0; k < 4000; k++) {
                float a = *(volatile float *)ENG(0x1099538);
                float b = *(volatile float *)ENG(0x109b898);
                if (a > max_ft) max_ft = a;
                if (b > max_aft) max_aft = b;
                sceKernelDelayThread(50);
            }
            float    m_scale    = *(volatile float *)ENG(0x1031ebc);     // Metrics::mScale
            float    fixed_step = *(volatile float *)ENG(0x109b148);     // Metrics::mFixedTimeStep
            uint32_t tgt_value  = *(volatile uint32_t *)ENG(0x109c5d8);  // Metrics::mTimeGetTimeValue
            logf_line("[watchdog] timing: maxFrameTime=%d us maxActualFrameTime=%d us mScale*1000=%d "
                      "fixedTimeStep*1e6=%d timeGetTimeValue=%u SDL_GetTicks=%u\n",
                      (int)(max_ft * 1e6f), (int)(max_aft * 1e6f), (int)(m_scale * 1000.0f),
                      (int)(fixed_step * 1e6f), (unsigned)tgt_value, (unsigned)ticks_ms());
            uint32_t fx_calls   = *(volatile uint32_t *)ENG(0x1067b24);  // HandleObjectInfo::smCurrentLockFrame (++ per RenderFX)
            int32_t  fx_guard   = *(volatile int32_t *)ENG(0x1097494);   // RenderFX re-entry guard (must be 0 between frames)
            uint32_t busy228    = ctx ? *(volatile uint32_t *)(ctx + 0x228) : 0; // BeginFrame fails if != 0
            uint32_t slot0      = ctx ? *(volatile uint32_t *)(ctx + 0x1c8) : 0; // frame slot[0]
            uint32_t slot0_44   = slot0 ? *(volatile uint32_t *)(slot0 + 0x44) : 0;
            logf_line("[watchdog] renderfx: calls=%u guard=%d ctx+0x228=%u slot0=0x%X slot0+0x44=0x%X\n",
                      (unsigned)fx_calls, (int)fx_guard, (unsigned)busy228, (unsigned)slot0,
                      (unsigned)slot0_44);
            if (ctx) {
                volatile uint8_t *c = (volatile uint8_t *)ctx;
                logf_line("[watchdog] renderq: queued(0x1f4)=%u head(0x1f8)=0x%X inflight(0x220)=%d "
                          "frameCounter(0x210)=%u flags 0x22c..0x230 = %u %u %u %u %u | sema 0x214 uid=0x%X\n",
                          *(volatile uint32_t *)(c + 0x1f4), *(volatile uint32_t *)(c + 0x1f8),
                          *(volatile int32_t *)(c + 0x220), *(volatile uint32_t *)(c + 0x210),
                          c[0x22c], c[0x22d], c[0x22e], c[0x22f], c[0x230],
                          *(volatile uint32_t *)(c + 0x214));
            }
            logf_line("[watchdog] engine: mtRender=%u suspended=%u frameNum=%u frameTime=%d us "
                      "actualFrameTime=%d us renderCtx=0x%X renderFrameIdx=%u | RenderFX gates: "
                      "reInitDevice=%u enableRendering=%u renderDelayFrames=%d gameWindow=0x%X\n",
                      mt_enable, suspended, (unsigned)frame_num, (int)(frame_time * 1e6f),
                      (int)(actual_ft * 1e6f), (unsigned)ctx, (unsigned)rframe_idx,
                      reinit, enable_r, (int)delay_fr, (unsigned)game_win);
        }
        if ((s + 1) % 10 == 0)
            logf_line("[watchdog] %d s, no frame yet. pread: %u calls, %u MB | main loop "
                      "(SDL_PollEvent): %u | GL: %u clears, %u draws, %u FBO binds, last GL "
                      "thread 0x%X (main 0x%X)\n", s + 1,
                      (unsigned)pread_calls, (unsigned)(pread_bytes >> 20),
                      (unsigned)poll_calls, (unsigned)gl_clears, (unsigned)gl_draws,
                      (unsigned)gl_fbo_binds, (unsigned)gl_last_thread, (unsigned)main_thread_id);
    }
    logf_line("[watchdog] no frame after %d s -> forcing a crash dump to see where "
              "the game is stuck\n", WATCHDOG_SECONDS);
    sceKernelDelayThread(200 * 1000); // let the log line reach the file
    *(volatile int *)0 = 0xDEAD;      // intentional crash
    return 0;
}

static void start_watchdog(void) {
    static int started = 0;
    if (started) return;
    started = 1;
    SceUID t = sceKernelCreateThread("mcsm_watchdog", watchdog_thread, 0x10000100,
                                     64 * 1024, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

static void pio_lock_init(void);

// GL call counters (diagnostics): do frames get drawn at all, and by whom?
static void glClear_cnt(GLbitfield mask) {
    gl_clears++; gl_last_thread = sceKernelGetThreadId();
    glClear(mask);
}
static void glDrawElements_cnt(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    gl_draws++; gl_last_thread = sceKernelGetThreadId();
    glDrawElements(mode, count, type, idx);
}
static void glDrawArrays_cnt(GLenum mode, GLint first, GLsizei count) {
    gl_draws++; gl_last_thread = sceKernelGetThreadId();
    glDrawArrays(mode, first, count);
}
static void glBindFramebuffer_cnt(GLenum target, GLuint fb) {
    gl_fbo_binds++;
    glBindFramebuffer(target, fb);
}

static int SDL_Init_impl(uint32_t flags) {
    LOG_FIRST("SDL_Init(0x%X)", (unsigned int)flags);
    pio_lock_init();
    main_thread_id = sceKernelGetThreadId();
    start_watchdog();
    return 0;
}

static void SDL_Quit_impl(void) {
    LOG_FIRST("SDL_Quit()");
}

static int SDL_SetHint_impl(const char *name, const char *value) {
    logf_line("[sdl] SDL_SetHint(%s = %s)\n", name ? name : "null", value ? value : "null");
    return 1; // SDL_TRUE
}

static const char *SDL_GetError_impl(void) {
    return "";
}

static void SDL_Log_impl(const char *fmt, ...) {
    char msg[480];
    va_list list;
    va_start(list, fmt);
    vsnprintf(msg, sizeof(msg), fmt, list);
    va_end(list);
    logf_line("[game] %s\n", msg); // the engine's own messages
}

static int SDL_ShowCursor_impl(int toggle) {
    return 0;
}

static int SDL_GetDisplayBounds_impl(int index, SDL_Rect *rect) {
    LOG_FIRST("SDL_GetDisplayBounds(%d) -> 960x544", index);
    if (rect) { rect->x = 0; rect->y = 0; rect->w = 960; rect->h = 544; }
    return 0;
}

static void *SDL_CreateWindow_impl(const char *title, int x, int y, int w, int h, uint32_t flags) {
    logf_line("[sdl] SDL_CreateWindow(\"%s\", %dx%d, flags 0x%X)\n",
              title ? title : "", w, h, (unsigned int)flags);
    return &fake_window;
}

static void SDL_DestroyWindow_impl(void *window) {
    LOG_FIRST("SDL_DestroyWindow()");
}

static uint32_t SDL_GetWindowID_impl(void *window) {
    return 1;
}

static int SDL_GL_SetAttribute_impl(int attr, int value) {
    LOG_FIRST("SDL_GL_SetAttribute(%d, %d)", attr, value);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* GL buffer diagnostics (crash in RenderUtility::Initialize: the engine   */
/* got NULL from glMapBuffer/glMapBufferRange). These wrappers call vitaGL */
/* exactly as before and only add log lines when something fails.         */
/* ---------------------------------------------------------------------- */

extern GLenum vgl_error; // vitaGL's pending glGetError() value (gxm.c)

static void log_gl_mem(const char *when) {
    logf_line("[gl] memory %s: free VRAM %u KB, RAM %u KB, PHYCONT %u KB, "
              "CDLG %u KB, newlib %u KB\n", when,
              (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024),
              (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
              (unsigned)(vglMemFree(VGL_MEM_SLOW) / 1024),
              (unsigned)(vglMemFree(VGL_MEM_BUDGET) / 1024),
              (unsigned)(vglMemFree(VGL_MEM_EXTERNAL) / 1024));
}

static GLint bound_buffer(GLenum target) {
    GLint id = 0;
    GLenum saved = vgl_error; // glGetIntegerv must not change the error state
    glGetIntegerv(target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING
                                                    : GL_ARRAY_BUFFER_BINDING, &id);
    vgl_error = saved;
    return id;
}

static void glBufferData_dbg(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    static int calls = 0;
    GLenum before = vgl_error;
    glBufferData(target, size, data, usage);
    calls++;
    if (vgl_error != GL_NO_ERROR && vgl_error != before) {
        logf_line("[gl] glBufferData FAILED: target 0x%X, buffer 0x%X, size %d, data %p, "
                  "usage 0x%X -> error 0x%X (call #%d)\n", target,
                  (unsigned)bound_buffer(target), (int)size, data, usage, vgl_error, calls);
        log_gl_mem("at failure");
    } else if (calls <= 3) {
        logf_line("[gl] glBufferData OK: target 0x%X, buffer 0x%X, size %d, usage 0x%X\n",
                  target, (unsigned)bound_buffer(target), (int)size, usage);
    }
}

static void *glMapBuffer_dbg(GLenum target, GLenum access) {
    void *p = glMapBuffer(target, access);
    if (!p)
        logf_line("[gl] glMapBuffer returned NULL: target 0x%X, buffer 0x%X, access 0x%X, "
                  "error 0x%X\n", target, (unsigned)bound_buffer(target), access, vgl_error);
    return p;
}

static void *glMapBufferRange_dbg(GLenum target, GLintptr offset, GLsizeiptr length,
                                  GLbitfield access) {
    void *p = glMapBufferRange(target, offset, length, access);
    if (!p)
        logf_line("[gl] glMapBufferRange returned NULL: target 0x%X, buffer 0x%X, "
                  "offset %d, length %d, access 0x%X, error 0x%X\n", target,
                  (unsigned)bound_buffer(target), (int)offset, (int)length, access, vgl_error);
    return p;
}

/* ---------------------------------------------------------------------- */
/* GL_EXTENSIONS filter                                                    */
/* vitaGL is GLES 2.0, so the engine's GL_MAJOR_VERSION check fails and it  */
/* takes its GLES2 path. On that path, seeing GL_EXT_map_buffer_range sets  */
/* render cap 22 without cap 23, and RenderDevice::MapGLBuffer() then       */
/* always returns NULL -> crash in RenderUtility::Initialize(). Hiding the  */
/* extension makes the engine use glMapBufferOES like Mali/Adreno GLES2     */
/* phones do.                                                               */
/* ---------------------------------------------------------------------- */

static const char *hidden_extensions[] = {
    "GL_EXT_map_buffer_range",
};

static const char *filtered_extensions(void) {
    static char *filtered = NULL;
    if (filtered)
        return filtered;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!ext)
        return NULL;
    filtered = malloc(strlen(ext) + 1);
    if (!filtered)
        return ext;
    char *out = filtered;
    const char *p = ext;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t len = p - start;
        if (!len)
            break;
        int hide = 0;
        for (size_t i = 0; i < sizeof(hidden_extensions) / sizeof(hidden_extensions[0]); i++) {
            if (strlen(hidden_extensions[i]) == len && !strncmp(start, hidden_extensions[i], len)) {
                hide = 1;
                logf_line("[gl] hiding extension %s from the engine\n", hidden_extensions[i]);
            }
        }
        if (!hide) {
            if (out != filtered) *out++ = ' ';
            memcpy(out, start, len);
            out += len;
        }
    }
    *out = '\0';
    return filtered;
}

static const GLubyte *glGetString_mcsm(GLenum name) {
    if (name == GL_EXTENSIONS) {
        const char *ext = filtered_extensions();
        if (ext)
            return (const GLubyte *)ext;
    }
    return glGetString(name);
}

// The engine fetches the map functions through eglGetProcAddress, so hand
// out the logging versions there too.
static void *eglGetProcAddress_dbg(const char *name) {
    if (name) {
        if (!strcmp(name, "glMapBuffer") || !strcmp(name, "glMapBufferOES"))
            return (void *)&glMapBuffer_dbg;
        if (!strcmp(name, "glMapBufferRange") || !strcmp(name, "glMapBufferRangeEXT"))
            return (void *)&glMapBufferRange_dbg;
        if (!strcmp(name, "glBufferData"))
            return (void *)&glBufferData_dbg;
        if (!strcmp(name, "glGetString"))
            return (void *)&glGetString_mcsm;
    }
    void *p = eglGetProcAddress(name);
    logf_line("[gl] eglGetProcAddress(%s) -> %p\n", name ? name : "null", p);
    return p;
}

static void *SDL_GL_CreateContext_impl(void *window) {
    logf_line("[sdl] SDL_GL_CreateContext() -> starting vitaGL\n");
    if (!sdl_gl_ready) {
        gl_init();
        sdl_gl_ready = 1;
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        logf_line("[sdl] vitaGL started. GL_VERSION: %s\n", (const char *)glGetString(GL_VERSION));
        logf_line("[sdl] GL_EXTENSIONS: %s\n", ext ? ext : "(none)");
        log_gl_mem("after vitaGL start");
    }
    return (void *)1;
}

static int SDL_GL_MakeCurrent_impl(void *window, void *context) {
    return 0;
}

static void SDL_GL_DeleteContext_impl(void *context) {
    LOG_FIRST("SDL_GL_DeleteContext()");
}

static void SDL_GL_SwapWindow_impl(void *window) {
    static int frames = 0;
    frames++;
    frames_presented = frames;
    if (frames == 1 || frames == 60 || frames == 600 || frames == 3600)
        logf_line("[sdl] frame %d presented\n", frames);
    gl_swap();
    input_needs_poll = 1;
}

static int SDL_GL_ExtensionSupported_impl(const char *name) {
    int supported = 0;
    if (sdl_gl_ready && name) {
        const char *ext = filtered_extensions(); // same list the engine sees
        if (ext) {
            size_t len = strlen(name);
            const char *p = ext;
            while ((p = strstr(p, name)) != NULL) { // whole-word match
                if ((p == ext || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
                    supported = 1;
                    break;
                }
                p += len;
            }
        }
    }
    logf_line("[sdl] SDL_GL_ExtensionSupported(%s) -> %s\n", name ? name : "null",
              supported ? "yes" : "no");
    return supported;
}

static int SDL_PollEvent_impl(SDL_Event *event) {
    poll_calls++;
    if (!devices_announced) {
        devices_announced = 1;
        SDL_DeviceEvent ev = { SDL_CONTROLLERDEVICEADDED, ticks_ms(), 0 };
        queue_push(&ev, sizeof(ev));
        SDL_DeviceEvent jev = { SDL_JOYDEVICEADDED, ticks_ms(), 0 };
        queue_push(&jev, sizeof(jev));
        logf_line("[sdl] announced the Vita controls as game controller #0\n");
    }
    if (input_needs_poll) {
        input_needs_poll = 0;
        poll_buttons_and_sticks();
        poll_touch();
    }
    return queue_pop(event);
}

static int SDL_PushEvent_impl(SDL_Event *event) {
    if (event) {
        LOG_FIRST("SDL_PushEvent(type 0x%X)", (unsigned int)event->type);
        queue_push(event, sizeof(SDL_Event));
    }
    return 1;
}

static uint32_t SDL_GetTicks_impl(void) {
    return ticks_ms();
}

static uint64_t SDL_GetPerformanceCounter_impl(void) {
    return sceKernelGetProcessTimeWide(); // microseconds
}

static uint64_t SDL_GetPerformanceFrequency_impl(void) {
    return 1000000;
}

static void SDL_Delay_impl(uint32_t ms) {
    sceKernelDelayThread(ms * 1000);
}

static int SDL_GetNumTouchDevices_impl(void) {
    return 1;
}

/* --- Joystick / game controller: one controller, the Vita itself --- */

static int SDL_NumJoysticks_impl(void) {
    LOG_FIRST("SDL_NumJoysticks() -> 1");
    return 1;
}

static int SDL_JoystickEventState_impl(int state) {
    return 1;
}

static int SDL_GameControllerEventState_impl(int state) {
    return 1;
}

static int SDL_GameControllerAddMapping_impl(const char *mapping) {
    return 1;
}

static void *SDL_GameControllerOpen_impl(int index) {
    logf_line("[sdl] SDL_GameControllerOpen(%d)\n", index);
    return (index == 0) ? &fake_controller : NULL;
}

static void SDL_GameControllerClose_impl(void *gc) {}

static void *SDL_GameControllerGetJoystick_impl(void *gc) {
    return &fake_controller;
}

static void *SDL_JoystickOpen_impl(int index) {
    logf_line("[sdl] SDL_JoystickOpen(%d)\n", index);
    return (index == 0) ? &fake_controller : NULL;
}

static void SDL_JoystickClose_impl(void *joy) {}

static const char *SDL_JoystickNameForIndex_impl(int index) {
    return "PS Vita Controller";
}

static const char *SDL_JoystickName_impl(void *joy) {
    return "PS Vita Controller";
}

static int SDL_JoystickGetAttached_impl(void *joy) {
    return 1;
}

static void SDL_JoystickUpdate_impl(void) {
    if (input_needs_poll) {
        input_needs_poll = 0;
        poll_buttons_and_sticks();
        poll_touch();
    }
}

static void SDL_GameControllerUpdate_impl(void) {
    SDL_JoystickUpdate_impl();
}

static uint8_t SDL_GameControllerGetButton_impl(void *gc, int button) {
    return (button >= 0 && button < BTN_COUNT) ? btn_state[button] : 0;
}

static int16_t SDL_GameControllerGetAxis_impl(void *gc, int axis) {
    return (axis >= 0 && axis < AXIS_COUNT) ? axis_state[axis] : 0;
}

static int SDL_JoystickNumButtons_impl(void *joy) { return BTN_COUNT; }
static int SDL_JoystickNumAxes_impl(void *joy)    { return AXIS_COUNT; }
static int SDL_JoystickNumHats_impl(void *joy)    { return 1; }

static uint8_t SDL_JoystickGetButton_impl(void *joy, int button) {
    return SDL_GameControllerGetButton_impl(joy, button);
}

static int16_t SDL_JoystickGetAxis_impl(void *joy, int axis) {
    return SDL_GameControllerGetAxis_impl(joy, axis);
}

static uint8_t SDL_JoystickGetHat_impl(void *joy, int hat) {
    uint8_t v = 0;
    if (btn_state[BTN_DPAD_UP])    v |= 0x01;
    if (btn_state[BTN_DPAD_RIGHT]) v |= 0x02;
    if (btn_state[BTN_DPAD_DOWN])  v |= 0x04;
    if (btn_state[BTN_DPAD_LEFT])  v |= 0x08;
    return v;
}

/* ====================================================================== */
/* SDL's Android helpers (normally inside the Android libSDL2.so)         */
/* ====================================================================== */

static void *SDL_AndroidGetJNIEnv_impl(void) {
    return &jni; // FalsoJNI's fake JNI environment
}

static const char *SDL_AndroidGetInternalStoragePath_impl(void) {
    LOG_FIRST("SDL_AndroidGetInternalStoragePath() -> ux0:data/mcsm");
    return "ux0:data/mcsm";
}

static int SDL_AndroidGetExternalStorageState_impl(void) {
    return 3; // SDL_ANDROID_EXTERNAL_STORAGE_READ | _WRITE
}

static int Android_JNI_SetupThread_impl(void) {
    return 1;
}

static void *eglGetCurrentDisplay_impl(void) {
    return (void *)1;
}

/* ====================================================================== */
/* Simple libc / POSIX implementations                                    */
/* ====================================================================== */

static int setpriority_impl(int which, int who, int prio) {
    return 0; // thread priorities are handled differently on the Vita
}

static int sem_close_impl(void *sem) {
    return 0;
}

static int gettid_impl(void) {
    return sceKernelGetThreadId();
}

// Android "struct statfs" (32-bit ARM layout)
typedef struct {
    uint32_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
} statfs_bionic;

static int statfs_impl(const char *path, statfs_bionic *buf) {
    // Pretend there is plenty of free space (4 GB)
    sceClibMemset(buf, 0, sizeof(*buf));
    buf->f_bsize  = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 1024 * 1024;
    buf->f_bfree  = 1024 * 1024;
    buf->f_bavail = 1024 * 1024;
    buf->f_namelen = 255;
    return 0;
}

// pread/pwrite are emulated as seek + read + seek back. Real pread is safe
// to call from several threads on the same fd at once (the engine's loader
// threads read the OBB this way), so serialize the emulation with a lock;
// otherwise two threads move the file position under each other and read
// the wrong part of the archive.
static SceKernelLwMutexWork pio_lock;
static int pio_lock_ready = 0;

static void pio_lock_init(void) {
    if (!pio_lock_ready) {
        sceKernelCreateLwMutex(&pio_lock, "mcsm_pread", 0, 0, NULL);
        pio_lock_ready = 1;
    }
}

static ssize_t pread_impl(int fd, void *buf, size_t count, off_t offset) {
    sceKernelLockLwMutex(&pio_lock, 1, NULL);
    off_t old = lseek(fd, 0, SEEK_CUR);
    ssize_t r = -1;
    if (lseek(fd, offset, SEEK_SET) >= 0) {
        r = read(fd, buf, count);
        lseek(fd, old, SEEK_SET);
    }
    pread_calls++;
    if (r > 0) pread_bytes += r;
    sceKernelUnlockLwMutex(&pio_lock, 1);
    return r;
}

static ssize_t pwrite_impl(int fd, const void *buf, size_t count, off_t offset) {
    sceKernelLockLwMutex(&pio_lock, 1, NULL);
    off_t old = lseek(fd, 0, SEEK_CUR);
    ssize_t r = -1;
    if (lseek(fd, offset, SEEK_SET) >= 0) {
        r = write(fd, buf, count);
        lseek(fd, old, SEEK_SET);
    }
    sceKernelUnlockLwMutex(&pio_lock, 1);
    return r;
}

struct iovec_bionic { void *iov_base; size_t iov_len; };

static ssize_t writev_impl(int fd, const struct iovec_bionic *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total ? total : -1;
        total += r;
    }
    return total;
}

static unsigned int alarm_impl(unsigned int seconds) {
    return 0;
}

static int getid_impl(void) {
    return 0; // getuid / geteuid / getgid / getegid
}

// Android "struct passwd" (32-bit layout)
typedef struct {
    char *pw_name;
    char *pw_passwd;
    uint32_t pw_uid;
    uint32_t pw_gid;
    char *pw_dir;
    char *pw_shell;
} passwd_bionic;

static passwd_bionic *getpwuid_impl(uint32_t uid) {
    static passwd_bionic pw = {
        "vita", "", 0, 0, "ux0:data/mcsm", ""
    };
    return &pw;
}

static int gethostbyname_r_impl(const char *name, void *ret, char *buf,
                                size_t buflen, void **result, int *h_errnop) {
    // No network services: always "host not found"
    if (result) *result = NULL;
    if (h_errnop) *h_errnop = 1; // HOST_NOT_FOUND
    return 0;
}

static void _exit_impl(int status) {
    logf_line("[mcsm] game called _exit(%d)\n", status);
    sceKernelExitProcess(status);
}

/* ====================================================================== */
/* Table                                                                  */
/* ====================================================================== */

#define S(name, fn) { name, (uintptr_t)&fn }

so_default_dynlib mcsm_dynlib[] = {
    // GL buffer diagnostics (see glBufferData_dbg above)
    S("glBufferData", glBufferData_dbg),
    S("glGetString", glGetString_mcsm),
    S("glClear", glClear_cnt),
    S("glDrawElements", glDrawElements_cnt),
    S("glDrawArrays", glDrawArrays_cnt),
    S("glBindFramebuffer", glBindFramebuffer_cnt),
    S("glMapBuffer", glMapBuffer_dbg),
    S("glMapBufferOES", glMapBuffer_dbg),
    S("glMapBufferRange", glMapBufferRange_dbg),
    S("glMapBufferRangeEXT", glMapBufferRange_dbg),
    S("eglGetProcAddress", eglGetProcAddress_dbg),
    // SDL2
    S("SDL_Init", SDL_Init_impl),
    S("SDL_Quit", SDL_Quit_impl),
    S("SDL_SetHint", SDL_SetHint_impl),
    S("SDL_GetError", SDL_GetError_impl),
    S("SDL_Log", SDL_Log_impl),
    S("SDL_ShowCursor", SDL_ShowCursor_impl),
    S("SDL_GetDisplayBounds", SDL_GetDisplayBounds_impl),
    S("SDL_CreateWindow", SDL_CreateWindow_impl),
    S("SDL_DestroyWindow", SDL_DestroyWindow_impl),
    S("SDL_GetWindowID", SDL_GetWindowID_impl),
    S("SDL_GL_SetAttribute", SDL_GL_SetAttribute_impl),
    S("SDL_GL_CreateContext", SDL_GL_CreateContext_impl),
    S("SDL_GL_MakeCurrent", SDL_GL_MakeCurrent_impl),
    S("SDL_GL_DeleteContext", SDL_GL_DeleteContext_impl),
    S("SDL_GL_SwapWindow", SDL_GL_SwapWindow_impl),
    S("SDL_GL_ExtensionSupported", SDL_GL_ExtensionSupported_impl),
    S("SDL_PollEvent", SDL_PollEvent_impl),
    S("SDL_PushEvent", SDL_PushEvent_impl),
    S("SDL_GetTicks", SDL_GetTicks_impl),
    S("SDL_GetPerformanceCounter", SDL_GetPerformanceCounter_impl),
    S("SDL_GetPerformanceFrequency", SDL_GetPerformanceFrequency_impl),
    S("SDL_Delay", SDL_Delay_impl),
    S("SDL_GetNumTouchDevices", SDL_GetNumTouchDevices_impl),
    S("SDL_NumJoysticks", SDL_NumJoysticks_impl),
    S("SDL_JoystickEventState", SDL_JoystickEventState_impl),
    S("SDL_GameControllerEventState", SDL_GameControllerEventState_impl),
    S("SDL_GameControllerAddMapping", SDL_GameControllerAddMapping_impl),
    S("SDL_GameControllerOpen", SDL_GameControllerOpen_impl),
    S("SDL_GameControllerClose", SDL_GameControllerClose_impl),
    S("SDL_GameControllerGetJoystick", SDL_GameControllerGetJoystick_impl),
    S("SDL_GameControllerUpdate", SDL_GameControllerUpdate_impl),
    S("SDL_GameControllerGetButton", SDL_GameControllerGetButton_impl),
    S("SDL_GameControllerGetAxis", SDL_GameControllerGetAxis_impl),
    S("SDL_JoystickOpen", SDL_JoystickOpen_impl),
    S("SDL_JoystickClose", SDL_JoystickClose_impl),
    S("SDL_JoystickNameForIndex", SDL_JoystickNameForIndex_impl),
    S("SDL_JoystickName", SDL_JoystickName_impl),
    S("SDL_JoystickGetAttached", SDL_JoystickGetAttached_impl),
    S("SDL_JoystickUpdate", SDL_JoystickUpdate_impl),
    S("SDL_JoystickNumButtons", SDL_JoystickNumButtons_impl),
    S("SDL_JoystickNumAxes", SDL_JoystickNumAxes_impl),
    S("SDL_JoystickNumHats", SDL_JoystickNumHats_impl),
    S("SDL_JoystickGetButton", SDL_JoystickGetButton_impl),
    S("SDL_JoystickGetAxis", SDL_JoystickGetAxis_impl),
    S("SDL_JoystickGetHat", SDL_JoystickGetHat_impl),

    // SDL's Android helpers
    S("SDL_AndroidGetJNIEnv", SDL_AndroidGetJNIEnv_impl),
    S("SDL_AndroidGetInternalStoragePath", SDL_AndroidGetInternalStoragePath_impl),
    S("SDL_AndroidGetExternalStorageState", SDL_AndroidGetExternalStorageState_impl),
    S("Android_JNI_SetupThread", Android_JNI_SetupThread_impl),
    S("eglGetCurrentDisplay", eglGetCurrentDisplay_impl),

    // libc / POSIX
    S("setpriority", setpriority_impl),
    S("sem_close", sem_close_impl),
    S("gettid", gettid_impl),
    S("statfs", statfs_impl),
    S("pread", pread_impl),
    S("pwrite", pwrite_impl),
    S("writev", writev_impl),
    S("alarm", alarm_impl),
    S("getuid", getid_impl),
    S("geteuid", getid_impl),
    S("getgid", getid_impl),
    S("getegid", getid_impl),
    S("getpwuid", getpwuid_impl),
    S("gethostbyname_r", gethostbyname_r_impl),
    S("_exit", _exit_impl),

    // Directly available in newlib
    S("asctime", asctime),
    S("tanhf", tanhf),
    S("modff", modff),
    S("coshf", coshf),
    // Bionic's sigjmp_buf is larger than newlib's jmp_buf, so plain
    // setjmp/longjmp fit (the signal mask argument is ignored).
    S("sigsetjmp", setjmp),
    S("siglongjmp", longjmp),
};

int mcsm_dynlib_count = sizeof(mcsm_dynlib) / sizeof(mcsm_dynlib[0]);
