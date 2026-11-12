# What the APK builds itself: libSDL2.so and libhookshot_adreno.so (libadrenotools, which loads a custom GPU driver, with
# its hook libraries). The game's own libmain.so and libhookshot_vk.so come built, in ../jniLibs/arm64-v8a.
cmake_minimum_required(VERSION 3.22)
project(hookshot_android LANGUAGES C CXX)

if(SDL2_SRC)   # (not given: the adrenotools part alone, as tests build it)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${SDL2_SRC} sdl2)   # (a plain target: Gradle packages the libraries of the targets it builds)
endif()

add_subdirectory(${ADRENOTOOLS_SRC} adrenotools)
add_library(hookshot_adreno SHARED hookshot_adreno.cpp)
target_link_libraries(hookshot_adreno PRIVATE adrenotools)
