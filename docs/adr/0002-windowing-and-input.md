# 0002: Windowing and input on SDL3, behind `foh::Window`

- Status: **Accepted** (project lead delegated the choice: "whatever fits the most for us")
- Related: README "Tech Stack", AGENTS.md rules 3 and 4, [ADR 0003](0003-renderer-render-graph-viewports.md)

## Context

The engine needs an OS window, an event pump, keyboard/mouse input, DPI handling, cursor and clipboard support for the editor UI, and a native handle for the swapchain. It must work on Windows first and stay portable to Linux/macOS. SDL3 was already a dependency (vcpkg, static).

## Decision

Keep **SDL3** and hide it completely behind `engine/platform` (`foh/window.h`). No SDL type appears in a public header.

- `Window::pollEvents()` pumps the OS and produces two views of the same input: a polled `InputState` snapshot (keys, mouse buttons, mouse position/delta, wheel; per-frame values reset every pump) for gameplay and cameras, and an ordered `WindowEvent` stream (`events()`) for UI layers (key down/up with repeat flag, UTF-8 text, mouse move/button/wheel, focus).
- Keys are physical (scancode based) engine enums, not SDL codes. Positions are logical window coordinates; `getSize` returns pixels (swapchain size), `logicalSize` returns points, so a high-DPI display works with `SDL_WINDOW_HIGH_PIXEL_DENSITY` and the UI divides by the framebuffer scale.
- UI support lives in the window too: `setCursor`, `setTextInput`, clipboard, `setRelativeMouse` (fly cameras), `isMinimized` (the renderer must skip frames, a swapchain cannot be zero sized).
- The previous `setRawEventCallback(SDL_Event*)` hook and `sdlWindow()` were removed on purpose (see below).

## Why not the stock `imgui_impl_sdl3` backend

SDL3 is linked **statically and privately** into `foh_engine.dll`. A second SDL copy in the editor executable would have its own global state (video subsystem, window list), and `imgui_impl_sdl3` would operate on windows that copy has never heard of. So the editor ships its own small ImGui platform glue (`editor/src/imgui_platform.cpp`, about 150 lines) that consumes the engine event stream. This also keeps rule 4 intact: only `platform/` knows SDL.

## Consequences

- (+) One owner of SDL state; the editor, runtime and tests cannot disagree about it.
- (+) Input is plain data, so it can be copied to other threads, recorded, or replayed.
- (+) Replacing SDL later (or adding a headless window for tests) touches only `platform/`.
- (-) We maintain the ImGui platform glue ourselves (keys, cursor, clipboard, text input). IME composition and gamepads are not handled yet.
- (-) ImGui multi-viewport (dragging panels out into their own OS windows) is not enabled: it needs a swapchain per window and ImGui platform callbacks for window creation. `Swapchain` is per window by design, so it can be added without restructuring. The editor's multiple *render* viewports are unrelated to this and work inside the one window (ADR 0003).
