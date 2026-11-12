/* (android) SDL's activity loads libmain.so and calls SDL_main. The game's folder (image.bin, romfs, user: what the builder
   makes on a PC, copied over) is the app's external files folder, or HOOKSHOT_DIR when the environment has one.
   A custom GPU driver (a Turnip build's .so, unpacked into the game folder's "driver" folder) is copied to the app's own
   storage, where code may be loaded from, and named in HOOKSHOT_VK_DRIVER for host/posix/win32_posix.c's LoadLibraryA. */
#ifdef __ANDROID__
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "hookshot", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hookshot", __VA_ARGS__)

int main(int argc, char **argv);   /* (hos_main.c) */

/* (by SDL's own handle: the activity loaded it with System.loadLibrary, which RTLD_DEFAULT does not search from here) */
static const char *sdl_path(const char *sdl2, const char *sdl3) {
    void *sdl = dlopen("libSDL2.so", RTLD_NOW);
    if (!sdl) sdl = dlopen("libSDL3.so", RTLD_NOW);
    const char *(*fn)(void) = sdl ? (const char *(*)(void))dlsym(sdl, sdl2) : NULL;
    if (!fn && sdl) fn = (const char *(*)(void))dlsym(sdl, sdl3);
    return fn ? fn() : NULL;
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    char buf[1 << 16];
    size_t n;
    int ok = in && out;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out) ok = fclose(out) == 0 && ok;
    return ok;
}

/* <game>/driver/<name>.so -> <internal>/driver/<name>.so, and the environment for LoadLibraryA("vulkan-1.dll") */
static void custom_driver(const char *game, const char *internal) {
    char from[1024], to[1024], name[256] = "";
    snprintf(from, sizeof from, "%s/driver", game);
    DIR *d = opendir(from);
    if (!d || !internal) {
        if (d) closedir(d);
        return;
    }
    const char *want = getenv("HOOKSHOT_VK_DRIVER");
    snprintf(to, sizeof to, "%s/driver", internal);
    mkdir(to, 0700);
    for (struct dirent *e; (e = readdir(d));) {
        const size_t n = strlen(e->d_name);
        if (n < 4 || n >= sizeof name || strcmp(e->d_name + n - 3, ".so")) continue;
        snprintf(from, sizeof from, "%s/driver/%s", game, e->d_name);
        snprintf(to, sizeof to, "%s/driver/%s", internal, e->d_name);
        if (!copy_file(from, to)) {
            LOGE("driver: cannot copy %s", from);
            continue;
        }
        chmod(to, 0700);
        if (want ? !strcmp(want, e->d_name) : !name[0]) snprintf(name, sizeof name, "%s", e->d_name);
    }
    closedir(d);
    if (!name[0]) return;
    Dl_info me;
    char libs[1024] = "";
    if (dladdr((void *)&custom_driver, &me) && me.dli_fname) {   /* (the APK's unpacked libraries: extractNativeLibs) */
        snprintf(libs, sizeof libs, "%s", me.dli_fname);
        char *slash = strrchr(libs, '/');
        if (slash) slash[1] = 0;
    }
    snprintf(to, sizeof to, "%s/driver/", internal);
    setenv("HOOKSHOT_VK_DRIVER", name, 1);
    setenv("HOOKSHOT_VK_DRIVER_DIR", to, 1);
    setenv("HOOKSHOT_NATIVE_LIB_DIR", libs, 1);
    LOGI("driver: %s%s (hooks in %s)", to, name, libs);
}

__attribute__((visibility("default"))) int SDL_main(int argc, char **argv) {
    (void)argc, (void)argv;
    if (!getenv("HOOKSHOT_DIR")) {
        const char *dir = sdl_path("SDL_AndroidGetExternalStoragePath", "SDL_GetAndroidExternalStoragePath");
        if (!dir) {
            LOGE("no external files folder: set HOOKSHOT_DIR");
            return 2;
        }
        setenv("HOOKSHOT_DIR", dir, 1);
    }
    const char *game = getenv("HOOKSHOT_DIR");
    LOGI("game folder: %s", game);
    chdir(game);
    custom_driver(game, sdl_path("SDL_AndroidGetInternalStoragePath", "SDL_GetAndroidInternalStoragePath"));
    /* (an app's stderr is /dev/null, and a valid one: the game opens hookshot_log.txt only when it has none. Opened here,
       so what the game says, and why it ends, is in the file the start screen shows) */
    if (freopen("hookshot_log.txt", "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    char *args[] = {"hookshot", NULL};
    const int r = main(1, args);
    LOGI("the game returned %d (hookshot_log.txt in the game folder has the rest)", r);
    return r;
}
#endif
