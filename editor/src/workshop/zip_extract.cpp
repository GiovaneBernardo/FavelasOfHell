#include "zip_extract.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <miniz.h>

#include "foh/asset_container.h" // ReadFileBytes
#include "foh/types.h"
#include "foh/utf8_path.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

bool isBlockedExtension(const std::string& lowerName) {
    static const char* kBlocked[] = {".exe", ".dll", ".bat", ".cmd", ".com", ".scr", ".msi", ".ps1", ".vbs", ".js",
                                     ".jar", ".lnk", ".sh",  ".so",  ".dylib", ".app", ".pif", ".reg"};
    for (const char* ext : kBlocked) {
        const std::string e(ext);
        if (lowerName.size() >= e.size() && lowerName.compare(lowerName.size() - e.size(), e.size(), e) == 0) return true;
    }
    return false;
}

bool isReservedWindowsName(std::string stem) {
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                      "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* r : kReserved)
        if (stem == r) return true;
    return false;
}

struct WriteCtx {
    std::ofstream* out = nullptr;
    std::uint64_t fileBytes = 0;
    std::uint64_t* totalBytes = nullptr;
    const ZipLimits* limits = nullptr;
    bool exceeded = false;
};

size_t onWrite(void* user, mz_uint64, const void* data, size_t n) {
    auto* c = static_cast<WriteCtx*>(user);
    c->fileBytes += n;
    *c->totalBytes += n;
    if (c->fileBytes > c->limits->maxFileBytes || *c->totalBytes > c->limits->maxTotalBytes) {
        c->exceeded = true;
        return 0;
    }
    c->out->write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return *c->out ? n : 0;
}

} // namespace

// Returns the cleaned '/' separated relative path, or an empty string and a reason.
std::string SanitizeRelativePath(std::string name, std::string& why) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty()) {
        why = "empty name";
        return {};
    }
    if (name[0] == '/' || (name.size() > 1 && name[1] == ':')) {
        why = "absolute path";
        return {};
    }
    std::string out;
    size_t pos = 0;
    while (pos <= name.size()) {
        size_t slash = name.find('/', pos);
        if (slash == std::string::npos) slash = name.size();
        const std::string part = name.substr(pos, slash - pos);
        pos = slash + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            why = "path escapes the destination";
            return {};
        }
        if (part.find_first_of("<>:\"|?*") != std::string::npos ||
            std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; })) {
            why = "invalid characters";
            return {};
        }
        if (part.back() == ' ' || part.back() == '.') {
            why = "invalid trailing character";
            return {};
        }
        if (isReservedWindowsName(part.substr(0, part.find('.')))) {
            why = "reserved name";
            return {};
        }
        if (!out.empty()) out += '/';
        out += part;
    }
    if (out.empty()) why = "empty name";
    return out;
}

ZipResult ExtractZip(const fs::path& archive, const fs::path& destination, const ZipLimits& limits) {
    ZipResult result;
    std::error_code ec;
    const auto size = fs::file_size(archive, ec);
    if (ec) {
        result.error = "cannot read the archive";
        return result;
    }
    if (size > limits.maxArchiveBytes) {
        result.error = "archive is too large";
        return result;
    }
    std::vector<u8> bytes;
    if (!ReadFileBytes(archive, bytes) || bytes.empty()) {
        result.error = "cannot read the archive";
        return result;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
        result.error = std::string("not a valid zip file: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip));
        return result;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > limits.maxEntries) {
        mz_zip_reader_end(&zip);
        result.error = "archive has too many entries";
        return result;
    }
    fs::create_directories(destination, ec);
    const fs::path root = fs::weakly_canonical(destination, ec);
    std::uint64_t total = 0;

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (st.m_is_directory) continue;
        if (!st.m_is_supported) {
            result.skipped.push_back(std::string(st.m_filename) + ": unsupported compression or encryption");
            continue;
        }
        std::string why;
        const std::string rel = SanitizeRelativePath(st.m_filename, why);
        if (rel.empty()) {
            result.skipped.push_back(std::string(st.m_filename) + ": " + why);
            continue;
        }
        std::string lower = rel;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (isBlockedExtension(lower)) {
            result.skipped.push_back(rel + ": executable or script files are not extracted");
            continue;
        }
        const fs::path target = fs::weakly_canonical(root / PathFromUtf8(rel), ec);
        // Belt and braces after the string checks: the resolved path must still be inside the destination.
        const auto rel2 = target.lexically_relative(root);
        if (ec || rel2.empty() || *rel2.begin() == "..") {
            result.skipped.push_back(rel + ": path escapes the destination");
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.skipped.push_back(rel + ": cannot create file");
            continue;
        }
        WriteCtx ctx;
        ctx.out = &out;
        ctx.totalBytes = &total;
        ctx.limits = &limits;
        const bool ok = mz_zip_reader_extract_to_callback(&zip, i, &onWrite, &ctx, 0) != 0;
        out.close();
        if (!ok) {
            fs::remove(target, ec);
            if (ctx.exceeded) {
                mz_zip_reader_end(&zip);
                result.error = "archive expands beyond the allowed size";
                return result;
            }
            result.skipped.push_back(rel + ": corrupt entry");
            continue;
        }
        result.files.push_back(rel);
    }
    mz_zip_reader_end(&zip);
    result.bytesWritten = total;
    result.ok = true;
    if (result.files.empty()) {
        result.ok = false;
        result.error = "the archive contained no usable files";
    }
    return result;
}

} // namespace foh
