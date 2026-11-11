// libadrenotools behind one plain function, which host/posix/win32_posix.c finds by name (LoadLibraryA("vulkan-1.dll") on
// Android): the system's Vulkan loader, opened with the custom driver driverDir/driverName in place of the device's own.
#include <dlfcn.h>
#include <adrenotools/driver.h>

extern "C" __attribute__((visibility("default")))
void *hookshot_open_vulkan(const char *hookLibDir, const char *driverDir, const char *driverName) {
    return adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hookLibDir, driverDir, driverName, nullptr, nullptr);
}
