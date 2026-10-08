#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "foh/platform.h"

namespace foh {

// Hand-offs to the operating system's shell (file manager, default applications, file dialogs).
// Everything here is best effort: a false return means the OS refused or no handler exists.
//
// Thread-safety: main thread only, except that dialog results may arrive from another thread
// internally (TakeOpenFilesResult is the synchronized way to read them).

// Opens the file manager with `path` selected (or the folder itself when `path` is a directory).
FOH_API bool RevealInFileManager(const std::filesystem::path& path);
// Opens a file with its default application, or a http(s) URL in the browser.
FOH_API bool OpenWithDefaultApp(const std::filesystem::path& path);
FOH_API bool OpenUrl(const std::string& url);

// Asynchronous multi-select "open files" dialog. `patterns` is a ';' separated extension list
// without dots ("gltf;glb;fbx;png"). Poll TakeOpenFilesResult() once per frame for the answer.
FOH_API void ShowOpenFilesDialog(const std::string& filterName, const std::string& patterns,
                                 const std::filesystem::path& startFolder);
// True once per finished dialog (also when it was cancelled: `files` is then empty).
FOH_API bool TakeOpenFilesResult(std::vector<std::string>& files);

} // namespace foh
