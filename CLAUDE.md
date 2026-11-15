# Minecraft: Story Mode → PS Vita port (so-loader)

Context for Claude Code. The owner (HZ) is new to Vita porting: give step-by-step
instructions one action at a time, and when changing code, give full files.

## What this is
A loader that runs the Android ARMv7 build of Minecraft: Story Mode (Telltale Tool
engine, 2015_07_Minecraft branch) on the PS Vita, based on
v-atamanenko/soloader-boilerplate (so_util + FalsoJNI + vitaGL).

## Build
Always switch to the softfp toolchain first (normal ~/vitasdk is for other projects):
```
source ~/softfp.sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```
Output: build-debug/mcsm.vpk (install with VitaShell). Title ID MCSM00001.

## Files on the Vita
- ux0:data/mcsm/ : libGameEngine.so, libfmod.so, libfmodstudio.so (from the APK's lib/armeabi-v7a)
- ux0:data/mcsm/log.txt : full log of every run (loader, SDL shim, JNI, shaders)
- ux0:data/mcsm/glsl/ : original GLSL shaders, gxp/ : compiled shader cache
- Crash dumps: ux0:data/psp2core-*.psp2dmp → parse with vita-parse-core using build-debug/so_loader

## How the port is put together
- source/utils/init.c : loads 3 libs. libGameEngine @0x98000000, libfmod @0x99200000,
  libfmodstudio @0x99600000 (0x90000000 collided with the 256 MB heap). Writes a
  missing-imports report to log.txt.
- source/main.c : calls the static `Application_SDL::Run()` (`_ZN15Application_SDL3RunEv`)
  directly (libmain.so's SDL_main just tail-calls it). Main thread stack 8 MB.
- source/mcsm/imports.c : "SDL shim", our own implementation of the ~45 SDL2 functions
  the engine uses (window/context → vitaGL, Vita pad → SDL game controller #0,
  front touch → SDL finger events), SDL Android helpers, small libc/POSIX functions.
  Joined with dynlib.c's table in resolve_imports() (dynlib.c).
- source/java.c : FalsoJNI answers (getHardwareModel, hasFeature, setFramebufferSize,
  getXDPI/YDPI=220, getSampleRate=48000, getObbFileName, storage dirs, etc.).
- source/patch.c : boilerplate kuser patch DISABLED. The engine contains 0xFFFF0FA0/FC0
  as normal GOT offsets, and patching them crashed Meta::Initialize.
- source/utils/logger.c : every log line is also written to ux0:data/mcsm/log.txt.
- lib/falso_jni/FalsoJNI_Logger.c : JNI log lines go to log.txt too.
- source/utils/glutil.c : logs shader compile success/failure, saves GLSL sources.

## Status (Nov 2026)
Engine boots, creates the vitaGL context, initializes audio (FMOD) and reaches
shader compilation. Next: get shaders compiling, then place the game data
(.ttarch2 archives from the phone's Android/data/.../files/Net plus the OBBs) and
find which paths the engine opens.

## Game data facts
.ttarch2 = TTCZ (zlib 64 KB chunks) or TTCN (uncompressed), unencrypted, inner TTA4.
Mali texture set: ETC1 + 16-bit + ARGB8, with no ETC2 (all native on the Vita GPU).
