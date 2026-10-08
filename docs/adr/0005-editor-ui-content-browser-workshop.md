# 0005: Editor UI scale, content browser and assets workshop

- Status: **Accepted** (project lead request, 2026-10-08), implemented.
- Related: AGENTS.md rules 1, 4, 5, 7; [ADR 0002](0002-windowing-and-input.md); [ADR 0004](0004-model-and-texture-import.md)

## Context

On a 4K monitor the editor was unreadable (ImGui's 13 px bitmap font, no DPI handling), and the asset panel was a flat tree of source files. The lead asked for an Unreal-style file explorer with previews and for a window that downloads free assets (ambientCG, Sketchfab, FreePBR, ...) with correct handling of zip packages.

## Decisions

### 1. One UI scale, derived from the display

`EditorTheme` computes `scale = Window::contentScale() / framebufferScale` (snapped to 1/8) and applies it to a freshly built style (`ScaleAllSizes`) and to `style.FontScaleDpi`; ImGui 1.92 rasterizes the font at the final size, so text stays sharp at any scale. The font is a system UI font (Segoe UI on Windows; DejaVu/SF fallbacks) loaded from disk, never bundled. When none exists ImGui's built-in font is used. Custom-drawn widgets multiply their pixel sizes by `EditorTheme::scale()`.

### 2. Content browser

- Folder tree + grid/list over the asset source root. Tiles show the name and the extension; files with a previewable type show a thumbnail, all others a typed icon with the extension printed on it (no icon assets needed).
- Thumbnails are produced on the job system and packed into shared atlas textures (`ThumbnailService`): images are decoded and box-filtered; imported models are **software-rasterized on the CPU** from their cooked prefab. No GPU context is needed, the render graph is untouched and nothing can block the frame. Only visible tiles are requested (clipped grid, stale queued requests are dropped), uploads are capped per frame, and the atlas evicts least recently drawn tiles.
- Directory listings are cached and refreshed on folder mtime changes; recursive search uses a capped, cached index.
- Operations: new folder, rename, delete (confirmed, `.meta` follows), move by drag onto a folder, reveal in the OS file manager, import dialog. Dragging a model onto a viewport places it where it is dropped. Preferences (zoom, split, folder) live in `editor_prefs.txt` next to the executable.
- OS hand-offs (reveal, open URL, file dialog) live in `engine/platform` (`foh/shell.h`), keeping platform code behind the platform layer.

### 3. Assets workshop

- `WorkshopProvider` implementations talk to documented free APIs: **ambientCG** (CC0, zip packages), **Poly Haven** (CC0, individual files incl. multi-file glTF), **Sketchfab** (search is open; downloads need the user's own API token). **FreePBR has no public API and does not permit scraping**, so it is deliberately not included; a new source is one class.
- Transport is **libcurl** (multi interface, driven from the main thread each frame: no threads, no blocking) restricted to https including redirects, with size caps, stall timeouts and `.part` files. API tokens are sent only to the API host, never to download URLs.
- Zip packages are extracted on the job system by **miniz** with zip-slip protection, rejection of absolute/reserved/invalid names and of executables and scripts, and bounded entry count and expanded size. Skipped entries are reported in the UI.
- Every package lands in `assets/workshop/<source>/<asset>_<variant>/` with an `ATTRIBUTION.txt` (author, license, URL, date; non-commercial licenses are flagged), then its models (or texture set) go through the normal importer. Nothing is loaded from the download directly (AGENTS rule 5).

## Dependencies added (editor feature only)

| Package | License | Use |
| --- | --- | --- |
| curl (schannel on Windows) | curl (MIT-like) | HTTPS for the workshop |
| miniz | MIT | zip extraction |

stb_image is also compiled into the editor (its engine copy is private to `foh_engine.dll`) for thumbnails. The JSON reader is our own (`editor/src/workshop/json.*`).

## Materials from texture sets

A downloaded PBR set becomes an authored material (`<name>.fmat`, `AssetDb::saveMaterial`) next to its textures: files are matched by the last word of their name (color / normal GL / roughness / metalness / AO / packed ARM), separate AO + roughness + metalness maps are packed into one glTF-style ORM texture on a low-priority job, the textures are imported, and the material is written when the last import ticket completes. Dragging a `.fmat` from the content browser onto an object (pick under the cursor) sets `MeshRenderer::material`; "Apply to selected object" does the same for the selection.

## Known limits / follow-ups

- Import is still synchronous on the main thread (a parallel task covers async import); the workshop imports one file per frame so the UI keeps updating.
- Thumbnails of materials and of single sub-assets (virtual folders under a model) are not done yet; neither is a disk cache for thumbnails.
- The Sketchfab token is stored in plain text in `editor_prefs.txt`.
