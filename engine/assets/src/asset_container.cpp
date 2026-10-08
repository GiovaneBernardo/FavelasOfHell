#include "foh/asset_container.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <random>

#include <lz4.h>
#include <zstd.h>

namespace foh {

static_assert(sizeof(void*) == 8, "the container assumes a 64-bit little endian host");

namespace {

constexpr u64 kP1 = 11400714785074694791ull;
constexpr u64 kP2 = 14029467366897019727ull;
constexpr u64 kP3 = 1609587929392839161ull;
constexpr u64 kP4 = 9650029242287828579ull;
constexpr u64 kP5 = 2870177450012600261ull;

inline u64 rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
inline u64 read64(const u8* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}
inline u32 read32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
inline u64 xxRound(u64 acc, u64 in) { return rotl(acc + in * kP2, 31) * kP1; }
inline u64 xxMerge(u64 acc, u64 v) { return (acc ^ xxRound(0, v)) * kP1 + kP4; }

constexpr size_t kHeaderSize = 64;
constexpr size_t kEntrySize = 48;
constexpr size_t kChunkAlign = 64;
constexpr u32 kMaxChunks = 4096;
constexpr u64 kMaxChunkRawSize = 1ull << 31;

size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

template <class T>
void put(std::vector<u8>& buf, size_t offset, T value) {
    std::memcpy(buf.data() + offset, &value, sizeof(T));
}
template <class T>
T get(const std::vector<u8>& buf, size_t offset) {
    T value;
    std::memcpy(&value, buf.data() + offset, sizeof(T));
    return value;
}

const char kHex[] = "0123456789abcdef";

} // namespace

u64 HashBytes(const void* data, size_t size, u64 seed) {
    const u8* p = static_cast<const u8*>(data);
    const u8* end = p + size;
    u64 h;
    if (size >= 32) {
        u64 v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        const u8* limit = end - 32;
        do {
            v1 = xxRound(v1, read64(p));
            v2 = xxRound(v2, read64(p + 8));
            v3 = xxRound(v3, read64(p + 16));
            v4 = xxRound(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xxMerge(h, v1);
        h = xxMerge(h, v2);
        h = xxMerge(h, v3);
        h = xxMerge(h, v4);
    } else {
        h = seed + kP5;
    }
    h += static_cast<u64>(size);
    while (p + 8 <= end) {
        h ^= xxRound(0, read64(p));
        h = rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<u64>(read32(p)) * kP1;
        h = rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<u64>(*p) * kP5;
        h = rotl(h, 11) * kP1;
        ++p;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

std::string ToString(const AssetId& id) {
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        s[static_cast<size_t>(i)] = kHex[(id.hi >> (60 - 4 * i)) & 0xF];
        s[static_cast<size_t>(16 + i)] = kHex[(id.lo >> (60 - 4 * i)) & 0xF];
    }
    return s;
}

bool ParseAssetId(std::string_view text, AssetId& out) {
    if (text.size() != 32) return false;
    u64 words[2] = {0, 0};
    for (size_t i = 0; i < 32; ++i) {
        const char c = text[i];
        u64 digit;
        if (c >= '0' && c <= '9') digit = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<u64>(c - 'A' + 10);
        else return false;
        words[i / 16] = (words[i / 16] << 4) | digit;
    }
    out.hi = words[0];
    out.lo = words[1];
    return true;
}

AssetId GenerateAssetId() {
    static thread_local std::mt19937_64 rng(std::random_device{}() ^
                                            static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()));
    AssetId id;
    do {
        id.hi = rng();
        id.lo = rng();
    } while (!id.isValid());
    return id;
}

AssetId DeriveAssetId(const AssetId& parent, std::string_view key) {
    AssetId id;
    id.hi = HashString(key, parent.hi ^ 0xA55A5AA5C3C3C3C3ull);
    id.lo = HashString(key, parent.lo ^ 0x3C3C3C3CA55A5AA5ull);
    if (!id.isValid()) id.lo = 1;
    return id;
}

size_t CompressBound(ChunkCodec codec, size_t srcSize) {
    switch (codec) {
        case ChunkCodec::None: return srcSize;
        case ChunkCodec::Lz4:  return static_cast<size_t>(LZ4_compressBound(static_cast<int>(srcSize)));
        case ChunkCodec::Zstd: return ZSTD_compressBound(srcSize);
    }
    return srcSize;
}

// ---- writer --------------------------------------------------------------------------------

ContainerWriter::ContainerWriter(u32 typeId, u32 typeVersion, AssetId id)
    : m_typeId(typeId), m_typeVersion(typeVersion), m_id(id) {}

void ContainerWriter::addChunk(u32 tag, std::span<const u8> data, ChunkCodec codec) {
    Pending chunk;
    chunk.tag = tag;
    chunk.rawSize = data.size();
    chunk.hash = HashBytes(data);
    chunk.codec = ChunkCodec::None;

    if (codec != ChunkCodec::None && !data.empty()) {
        std::vector<u8> packed(CompressBound(codec, data.size()));
        size_t packedSize = 0;
        if (codec == ChunkCodec::Lz4 && data.size() < static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
            const int n = LZ4_compress_default(reinterpret_cast<const char*>(data.data()),
                                               reinterpret_cast<char*>(packed.data()), static_cast<int>(data.size()),
                                               static_cast<int>(packed.size()));
            packedSize = n > 0 ? static_cast<size_t>(n) : 0;
        } else if (codec == ChunkCodec::Zstd) {
            const size_t n = ZSTD_compress(packed.data(), packed.size(), data.data(), data.size(), 3);
            packedSize = ZSTD_isError(n) ? 0 : n;
        }
        if (packedSize != 0 && packedSize < data.size()) {
            packed.resize(packedSize);
            chunk.stored = std::move(packed);
            chunk.codec = codec;
        }
    }
    if (chunk.codec == ChunkCodec::None) chunk.stored.assign(data.begin(), data.end());
    m_chunks.push_back(std::move(chunk));
}

std::vector<u8> ContainerWriter::finish() const {
    size_t cursor = kHeaderSize;
    std::vector<u64> offsets;
    for (const Pending& c : m_chunks) {
        cursor = alignUp(cursor, kChunkAlign);
        offsets.push_back(cursor);
        cursor += c.stored.size();
    }
    const size_t tableOffset = alignUp(cursor, 16);
    const size_t fileSize = tableOffset + m_chunks.size() * kEntrySize;

    std::vector<u8> file(fileSize, 0);
    put<u32>(file, 0, kContainerMagic);
    put<u32>(file, 4, kContainerVersion);
    put<u32>(file, 8, m_typeId);
    put<u32>(file, 12, m_typeVersion);
    put<u64>(file, 16, m_id.hi);
    put<u64>(file, 24, m_id.lo);
    put<u32>(file, 32, 0); // flags
    put<u32>(file, 36, static_cast<u32>(m_chunks.size()));
    put<u64>(file, 40, tableOffset);
    put<u64>(file, 48, fileSize);
    put<u64>(file, 56, HashBytes(file.data(), 56));

    for (size_t i = 0; i < m_chunks.size(); ++i) {
        const Pending& c = m_chunks[i];
        if (!c.stored.empty()) std::memcpy(file.data() + offsets[i], c.stored.data(), c.stored.size());
        const size_t e = tableOffset + i * kEntrySize;
        put<u32>(file, e + 0, c.tag);
        put<u32>(file, e + 4, static_cast<u32>(c.codec));
        put<u64>(file, e + 8, offsets[i]);
        put<u64>(file, e + 16, c.stored.size());
        put<u64>(file, e + 24, c.rawSize);
        put<u64>(file, e + 32, c.hash);
    }
    return file;
}

// ---- reader --------------------------------------------------------------------------------

std::optional<ContainerReader> ContainerReader::Open(std::vector<u8> file, std::string* error) {
    auto fail = [&](const char* message) -> std::optional<ContainerReader> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (file.size() < kHeaderSize) return fail("file smaller than the container header");
    if (get<u32>(file, 0) != kContainerMagic) return fail("bad magic (not a container)");
    if (get<u32>(file, 4) != kContainerVersion) return fail("unsupported container version");
    if (get<u64>(file, 56) != HashBytes(file.data(), 56)) return fail("header hash mismatch");
    const u64 fileSize = get<u64>(file, 48);
    if (fileSize != file.size()) return fail("file size does not match the header (truncated?)");
    const u32 count = get<u32>(file, 36);
    const u64 tableOffset = get<u64>(file, 40);
    if (count > kMaxChunks) return fail("too many chunks");
    if (tableOffset > fileSize || (fileSize - tableOffset) / kEntrySize < count) return fail("chunk table out of bounds");

    ContainerReader reader;
    reader.m_typeId = get<u32>(file, 8);
    reader.m_typeVersion = get<u32>(file, 12);
    reader.m_id.hi = get<u64>(file, 16);
    reader.m_id.lo = get<u64>(file, 24);
    for (u32 i = 0; i < count; ++i) {
        const size_t e = static_cast<size_t>(tableOffset) + i * kEntrySize;
        ChunkInfo info;
        info.tag = get<u32>(file, e + 0);
        const u32 codec = get<u32>(file, e + 4);
        if (codec > static_cast<u32>(ChunkCodec::Zstd)) return fail("unknown chunk codec");
        info.codec = static_cast<ChunkCodec>(codec);
        info.offset = get<u64>(file, e + 8);
        info.storedSize = get<u64>(file, e + 16);
        info.rawSize = get<u64>(file, e + 24);
        info.hash = get<u64>(file, e + 32);
        if (info.offset > tableOffset || info.storedSize > tableOffset - info.offset) return fail("chunk out of bounds");
        if (info.rawSize > kMaxChunkRawSize) return fail("chunk too large");
        if (info.codec == ChunkCodec::None && info.storedSize != info.rawSize) return fail("raw chunk size mismatch");
        reader.m_chunks.push_back(info);
    }
    reader.m_file = std::move(file);
    return reader;
}

const ChunkInfo* ContainerReader::findChunk(u32 tag) const {
    for (const ChunkInfo& c : m_chunks)
        if (c.tag == tag) return &c;
    return nullptr;
}

bool ContainerReader::readChunk(u32 tag, std::vector<u8>& out) const {
    const ChunkInfo* c = findChunk(tag);
    if (!c) return false;
    const u8* src = m_file.data() + c->offset;
    out.resize(static_cast<size_t>(c->rawSize));
    switch (c->codec) {
        case ChunkCodec::None:
            if (c->rawSize) std::memcpy(out.data(), src, static_cast<size_t>(c->rawSize));
            break;
        case ChunkCodec::Lz4: {
            const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(out.data()),
                                              static_cast<int>(c->storedSize), static_cast<int>(c->rawSize));
            if (n < 0 || static_cast<u64>(n) != c->rawSize) return false;
            break;
        }
        case ChunkCodec::Zstd: {
            const size_t n = ZSTD_decompress(out.data(), out.size(), src, static_cast<size_t>(c->storedSize));
            if (ZSTD_isError(n) || n != c->rawSize) return false;
            break;
        }
    }
    return HashBytes(out) == c->hash;
}

// ---- files ---------------------------------------------------------------------------------

bool ReadFileBytes(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in);
}

bool WriteFileBytes(const std::filesystem::path& path, std::span<const u8> data) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace foh
