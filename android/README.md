# Hookshot on Android (arm64): experimental, not yet run on a device

The Linux build's sources, cross-built with the Android NDK (`RX_TARGET=android`) into two libraries, inside an SDL2
activity's APK. The game's files are the ones a PC build makes: nothing of the game is in the APK.

State: the host layer and the Vulkan back end compile and link for `aarch64-linux-android33`; the arm64 fiber switch and
setjmp are tested under qemu. **Nothing has been run on a device.** Expect to work through start-up failures with
`adb logcat -s hookshot SDL` and `hookshot_log.txt`.

## What differs from Linux

| Area | Android |
|---|---|
| Guest window | 128 GB at host address 128 GB (`A64_HOST_BASE`, `HOS_ANDROID_SPACE` in `nx2/build.py`): a 39-bit address space cannot hold the 512 GB window at 16 TB |
| Fibers | `host/posix/fiber_arm64.h` (bionic has no `makecontext`); `tests/fiber_arm64_test.c` |
| setjmp in lifted code | arm64 `recomp_sj_setjmp` / `recomp_sj_longjmp` in `host/hos_setjmp.c` |
| Faults | pc, sp, fp and the write bit (ESR) from the arm64 signal context, `host/posix/win32_posix.c` |
| Libraries | `libmain.so` is linked `-z global` so the back end finds the host's symbols; all load by bare name from the APK: `libmain.so` (the game), `libhookshot_vk.so`, `libSDL2.so`, `libhookshot_adreno.so` |
| Game folder | `/sdcard/Android/data/io.github.hookshot/files` (`HOOKSHOT_DIR`), `host/posix/android_main.c` |
| GPU driver | the device's, or a Turnip `.so` in `<game folder>/driver/`, loaded through libadrenotools |
| Switches | `hookshot_env.txt` in the game folder, as elsewhere (an app has no environment) |

## Build

On the PC that built the Windows or Linux game from your copy, with the same work folder (the lift's C is reused; only
the compile is new).

1. Android NDK r27c or newer, Android Studio (or the command-line SDK with platform 34), an unpacked
   [SDL2 2.32 source release](https://github.com/libsdl-org/SDL/releases), and
   `git clone --recurse-submodules https://github.com/bylaws/libadrenotools`.
2. FFmpeg (libavcodec, libswscale, libswresample, libavutil) with dav1d, static, for Android arm64, in one prefix
   (`lib/libavcodec.a` ...). `scripts/linux/deps.sh` has the configure line the Linux build uses; add the NDK's
   `--cross-prefix`, `--target-os=android --arch=aarch64`.
3. Compile and link (PowerShell; `set` the same on Linux):

   ```
   $env:RX_TARGET = "android"
   $env:ANDROID_NDK_HOME = "C:\Android\ndk\27.2.12479018"
   $env:HOOKSHOT_FFMPEG = "C:\deps\ffmpeg-android-arm64"
   $env:OOT_WORK = "<the work folder of your PC build>"
   $env:RX_RELEASE = "1"
   python -I nx2/build.py --standalone
   ```

   Leave `RX_PGO` unset (the play profile is for LLVM 23 on x86-64). The result is `libmain.so` and
   `libhookshot_vk.so` in `<work>/sa/build`.
4. Copy both into `android/app/src/main/jniLibs/arm64-v8a/`.
5. `cd android`, set `SDL2_SRC` and `ADRENOTOOLS_SRC`, then `gradle assembleRelease` (or open the folder in Android
   Studio). Install with `adb install app/build/outputs/apk/release/app-release.apk`.

## On the device

1. Start the app once so its folder exists, then copy the PC game folder's contents (`image.bin`, `romfs`, and the rest,
   without the executables and DLLs) to `/sdcard/Android/data/io.github.hookshot/files/`.
2. Turnip: unpack the driver's `.so` into `files/driver/` (one driver: the first `.so` found is used).
3. Memory: the game is given 9809 MB as on the console. On a 12 GB device try `HOS_MEMORY_MB=7000` in `hookshot_env.txt` if
   Android kills the app; how low the game accepts is not known.

## Known gaps

- Not run on a device. First suspects: the guest window's `mmap` (another mapping in 128-256 GB), guest memory import
  (`VK_EXT_external_memory_host` must be in the driver's list: `vk:` lines in the log), SDL2's audio and pad paths.
- `nx2/make_game.py` does not know the target: the lift and the game folder come from a PC build.
- No updater, launcher or shader preparation step; the in-game menu's DLSS / FSR 4 entries are absent as on Linux.
- Touch input: none. A controller (the Thor's own pad) through SDL.
- 16 KB page devices: untested (the Thor's Android 13 uses 4 KB).
