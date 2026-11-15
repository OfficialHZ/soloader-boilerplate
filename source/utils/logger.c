/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * MCSM port: every log line is also written to DATA_PATH "log.txt",
 * so logs can be read on the Vita/PC without a network log viewer.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <stdbool.h>
#include <stdatomic.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

#define LOG_FILE_PATH DATA_PATH "log.txt"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Buffers for the plain-text (no colors) copy written to the log file.
static char buffer_fa[2048];
static char buffer_fb[2048];

// The log file is emptied on the first log line of every launch.
static bool _log_file_started = false;

static void _log_write_file(const char *text) {
    int flags = SCE_O_WRONLY | SCE_O_CREAT;
    if (_log_file_started) {
        flags |= SCE_O_APPEND;
    } else {
        flags |= SCE_O_TRUNC;
        _log_file_started = true;
    }

    SceUID fd = sceIoOpen(LOG_FILE_PATH, flags, 0777);
    if (fd < 0) return; // data folder may not exist yet; console log still works

    sceIoWrite(fd, text, sceClibStrnlen(text, sizeof(buffer_fb)));
    sceIoClose(fd);
}

// Public helper: write a raw line straight to the log file (and console).
void log_raw(const char *text) {
    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelLockLwMutex(&_log_mutex, 1, NULL);
    }
    sceClibPrintf("%s", text);
    _log_write_file(text);
    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    const char *label;
    switch (t) {
        case LT_DEBUG:
            label = "debug";
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            label = "info";
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            label = "warning";
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            label = "error";
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            label = "FATAL";
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            label = "success";
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            label = "waiting";
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            sceKernelUnlockLwMutex(&_log_mutex, 1);
            return;
    }

    // Colored copy for the network console
    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf("%s", buffer_b);

    // Plain copy for log.txt
    sceClibSnprintf(buffer_fa, sizeof(buffer_fa), "[%s] %s\n", label, fmt);
    va_start(list, fmt);
    sceClibVsnprintf(buffer_fb, sizeof(buffer_fb), buffer_fa, list);
    va_end(list);
    _log_write_file(buffer_fb);

    sceKernelUnlockLwMutex(&_log_mutex, 1);
}
