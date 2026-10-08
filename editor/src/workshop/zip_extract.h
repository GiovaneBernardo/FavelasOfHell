#pragma once

// Safe extraction of downloaded .zip archives (asset packs are untrusted input).
//
// Entries are validated before anything is written: no absolute paths, drive letters or ".."
// components (zip-slip), no reserved Windows names, no executables/scripts, bounded entry count and
// bounded expanded size (zip bombs). Rejected entries are reported, never silently dropped.
//
// Thread-safety: pure function, safe on a job thread.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace foh {

struct ZipLimits {
    std::uint64_t maxArchiveBytes = 1536ull << 20;
    std::uint64_t maxTotalBytes = 4096ull << 20; // expanded
    std::uint64_t maxFileBytes = 2048ull << 20;
    size_t maxEntries = 20000;
};

struct ZipResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> files;   // extracted, relative to the destination, '/' separators
    std::vector<std::string> skipped; // "<entry>: <reason>"
    std::uint64_t bytesWritten = 0;
};

// Normalizes an archive/download path to a clean relative '/' path. Empty (and `why` set) when it is unsafe:
// absolute, drive letters, "..", reserved or invalid names.
std::string SanitizeRelativePath(std::string name, std::string& why);

ZipResult ExtractZip(const std::filesystem::path& archive, const std::filesystem::path& destination,
                     const ZipLimits& limits = {});

} // namespace foh
