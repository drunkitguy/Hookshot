// libadrenotools behind plain functions, which the game finds by name (host/posix/win32_posix.c LoadLibraryA("vulkan-1.dll")
// and gpu/vk/vk_memory.cpp on Android).
// A custom driver (driverName): the system's Vulkan loader with that driver in place of the device's own.
// No custom driver: the device's own driver with libadrenotools' memory import. Adreno's driver has no
// VK_EXT_external_memory_host, which the renderer shares the game's memory with the GPU through; the import hands the
// game's pages to KGSL and has the driver's next allocation of that size be them (its libgsl allocation is hooked).
#include <dlfcn.h>
#include <adrenotools/driver.h>
#include <adrenotools/priv.h>   // (ADRENOTOOLS_DRIVER_*)

static void *mapping;   // (libadrenotools' handle for the import)

extern "C" __attribute__((visibility("default")))
void *hookshot_open_vulkan(const char *hookLibDir, const char *driverDir, const char *driverName) {
    if (driverName && driverName[0] && driverDir)
        return adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hookLibDir, driverDir, driverName, nullptr, nullptr);
    return adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT, nullptr, hookLibDir, nullptr, nullptr, nullptr, &mapping);
}

// 1 when the device's own driver was opened with the memory import
extern "C" __attribute__((visibility("default"))) int hookshot_import_ready(void) { return mapping != nullptr; }

// The pages at ptr are what the driver's next allocation of `size` bytes is made of. 0 when KGSL refuses them
extern "C" __attribute__((visibility("default"))) int hookshot_import_user_mem(void *ptr, unsigned long long size) {
    return mapping && adrenotools_import_user_mem(mapping, ptr, size);
}

// 1 when the allocation made since hookshot_import_user_mem took those pages. 0: the import is disarmed, else the hook hands
// the pages to the driver's next allocation of that size from any thread (its own command memory: the game wrote over it)
extern "C" __attribute__((visibility("default"))) int hookshot_import_taken(void) {
    if (!mapping) return 0;
    if (adrenotools_validate_gpu_mapping(mapping)) return 1;
    // (the hook matches on size and on the allocation's flags holding all of the mapping's: no 32-bit gsl flags hold ~0)
    static_cast<adrenotools_gpu_mapping *>(mapping)->size = 0;
    static_cast<adrenotools_gpu_mapping *>(mapping)->flags = ~0ull;
    return 0;
}
