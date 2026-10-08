#include "foh/rhi/shader_library.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "foh/log.h"

namespace foh {

namespace fs = std::filesystem;

namespace {

bool readFile(const fs::path& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<size_t>(size));
    in.seekg(0);
    return static_cast<bool>(in.read(out.data(), size));
}

struct FileStamp {
    fs::file_time_type time{};
    std::uintmax_t size = 0;
    bool operator==(const FileStamp&) const = default;
};

bool stampOf(const fs::path& path, FileStamp& out) {
    std::error_code ec;
    out.time = fs::last_write_time(path, ec);
    if (ec) return false;
    out.size = fs::file_size(path, ec);
    return !ec;
}

// Heterogeneous lookup: get() is called per pass per frame and must not allocate a std::string.
struct NameHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

} // namespace

struct ShaderLibrary::Impl {
    struct Entry {
        ShaderRef ref;
        nvrhi::ShaderType type = nvrhi::ShaderType::None;
        fs::path path;
        FileStamp loaded{};  // stamp of the bytes behind `ref`
        FileStamp pending{}; // last seen differing stamp, to wait until the writer is done
        bool reportedMissing = false;
    };

    nvrhi::IDevice* device;
    std::string extension;
    fs::path directory;
    std::mutex mutex;
    std::unordered_map<std::string, Entry, NameHash, std::equal_to<>> entries;
    std::atomic<u64> nextUid{1};
    std::atomic<u64> version{0};
    std::chrono::steady_clock::time_point lastScan{};

    bool load(Entry& e, const std::string& name) {
        std::vector<char> bytes;
        FileStamp stamp;
        if (!stampOf(e.path, stamp) || !readFile(e.path, bytes)) {
            if (!e.reportedMissing) {
                FOH_ERROR("shader binary not found: {} (was the 'foh_shaders' target built?)", e.path.string());
                e.reportedMissing = true;
            }
            return false;
        }
        nvrhi::ShaderDesc desc;
        desc.shaderType = e.type;
        desc.debugName = name;
        desc.entryName = "main";
        nvrhi::ShaderHandle shader = device->createShader(desc, bytes.data(), bytes.size());
        if (!shader) {
            FOH_ERROR("failed to create shader '{}' from {}", name, e.path.string());
            return false;
        }
        e.ref.shader = std::move(shader);
        e.ref.uid = nextUid.fetch_add(1);
        e.loaded = stamp;
        e.pending = stamp;
        e.reportedMissing = false;
        return true;
    }
};

ShaderLibrary::ShaderLibrary(nvrhi::IDevice* device, nvrhi::GraphicsAPI api, fs::path directory)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->device = device;
    m_impl->directory = std::move(directory);
    m_impl->extension = api == nvrhi::GraphicsAPI::VULKAN ? ".spv" : ".dxil";
}

ShaderLibrary::~ShaderLibrary() = default;

ShaderRef ShaderLibrary::get(std::string_view name, nvrhi::ShaderType type) {
    Impl& s = *m_impl;
    std::lock_guard lock(s.mutex);
    auto it = s.entries.find(name);
    if (it == s.entries.end()) {
        const std::string key(name);
        Impl::Entry e;
        e.type = type;
        e.path = s.directory / (key + s.extension);
        it = s.entries.emplace(key, std::move(e)).first;
        s.load(it->second, key);
    } else if (!it->second.ref && !it->second.reportedMissing) {
        s.load(it->second, std::string(name));
    }
    return it->second.ref;
}

bool ShaderLibrary::pollReload() {
    using Clock = std::chrono::steady_clock;
    Impl& s = *m_impl;
    const auto now = Clock::now();
    if (now - s.lastScan < std::chrono::milliseconds(250)) return false;
    s.lastScan = now;

    bool changed = false;
    std::lock_guard lock(s.mutex);
    for (auto& [name, e] : s.entries) {
        FileStamp stamp;
        if (!stampOf(e.path, stamp)) continue;
        if (stamp == e.loaded) continue;
        // Reload only once the stamp stopped changing between two scans: the
        // compiler may still be writing the file.
        if (!(stamp == e.pending)) {
            e.pending = stamp;
            continue;
        }
        const ShaderRef before = e.ref;
        if (s.load(e, name)) {
            FOH_INFO("reloaded shader {}", name);
            changed = true;
        } else {
            e.ref = before; // keep rendering with the last good version
            e.loaded = stamp;
        }
    }
    if (changed) s.version.fetch_add(1);
    return changed;
}

u64 ShaderLibrary::version() const { return m_impl->version.load(); }

} // namespace foh
