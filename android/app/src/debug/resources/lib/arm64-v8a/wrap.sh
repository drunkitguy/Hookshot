#!/system/bin/sh
# (the debug APK only: Android runs the app through this script when the app is debuggable) libc's malloc debug, off unless
# the property names its options: adb shell setprop debug.hookshot.malloc "backtrace guard free_track" (0 or empty: off).
# A heap overrun or a write into freed memory is then reported in logcat (malloc_debug) with the allocation's backtrace
opts=$(getprop debug.hookshot.malloc)
if [ -n "$opts" ] && [ "$opts" != 0 ]; then export LIBC_DEBUG_MALLOC_OPTIONS="$opts"; fi
exec "$@"
