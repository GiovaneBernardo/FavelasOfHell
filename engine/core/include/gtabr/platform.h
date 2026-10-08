#pragma once

// Platform / compiler detection and DLL export macros.

#if defined(_WIN32)
    #define GTABR_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define GTABR_PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define GTABR_PLATFORM_MACOS 1
#endif

// GTABR_API: engine DLL boundary. Exported while building gtabr_engine,
// imported by everything else (editor, runtime, game modules).
#if defined(_WIN32)
    #if defined(GTABR_ENGINE_BUILD)
        #define GTABR_API __declspec(dllexport)
    #else
        #define GTABR_API __declspec(dllimport)
    #endif
    #define GTABR_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
    #define GTABR_API __attribute__((visibility("default")))
    #define GTABR_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(_MSC_VER)
    #define GTABR_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
    #define GTABR_DEBUG_BREAK() __builtin_trap()
#else
    #define GTABR_DEBUG_BREAK() ((void)0)
#endif
