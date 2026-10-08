#pragma once

// Platform / compiler detection and DLL export macros.

#if defined(_WIN32)
    #define FOH_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define FOH_PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define FOH_PLATFORM_MACOS 1
#endif

// FOH_API: engine DLL boundary. Exported while building foh_engine,
// imported by everything else (editor, runtime, game modules).
#if defined(_WIN32)
    #if defined(FOH_ENGINE_BUILD)
        #define FOH_API __declspec(dllexport)
    #else
        #define FOH_API __declspec(dllimport)
    #endif
    #define FOH_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
    #define FOH_API __attribute__((visibility("default")))
    #define FOH_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(_MSC_VER)
    #define FOH_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
    #define FOH_DEBUG_BREAK() __builtin_trap()
#else
    #define FOH_DEBUG_BREAK() ((void)0)
#endif
