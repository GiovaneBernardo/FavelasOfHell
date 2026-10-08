#pragma once

// The chunked container shared by every asset type (README, "Assets", decision 2),
// plus the stable asset identity.
//
// File layout (little endian, chunk payloads 64-byte aligned so they can be
// memory-mapped and uploaded as they are):
//
//   Header (64 B)   magic, container version, type id + version, GUID, flags,
//                   chunk count, chunk table offset, file size, header hash
//   Chunk data...   each chunk: raw or LZ4/Zstd compressed
//   Chunk table     one 48 B entry per chunk: tag, codec, offset, stored size,
//                   raw size, hash of the raw bytes
//
// Readers validate every offset and size against the file size and verify the
// per-chunk hash on read: files are untrusted once mods or downloads exist.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "foh/asset_id.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// 64-bit non-cryptographic hash (XXH64), used for content hashes and cache keys.
FOH_API u64 HashBytes(const void* data, size_t size, u64 seed = 0);
inline u64 HashBytes(std::span<const u8> bytes, u64 seed = 0) { return HashBytes(bytes.data(), bytes.size(), seed); }
inline u64 HashString(std::string_view s, u64 seed = 0) { return HashBytes(s.data(), s.size(), seed); }

// 32 lowercase hex digits.
FOH_API std::string ToString(const AssetId& id);
FOH_API bool ParseAssetId(std::string_view text, AssetId& out);
FOH_API AssetId GenerateAssetId();
// Deterministic child id: the same (parent, key) always gives the same id, so a
// sub-asset keeps its identity even if the table that records it is lost.
FOH_API AssetId DeriveAssetId(const AssetId& parent, std::string_view key);

constexpr u32 FourCC(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | static_cast<u32>(static_cast<u8>(b)) << 8 |
           static_cast<u32>(static_cast<u8>(c)) << 16 | static_cast<u32>(static_cast<u8>(d)) << 24;
}

enum class ChunkCodec : u8 { None, Lz4, Zstd };

// Worst-case compressed size for a payload (used to size output buffers).
FOH_API size_t CompressBound(ChunkCodec codec, size_t srcSize);

inline constexpr u32 kContainerMagic = FourCC('F', 'O', 'H', 'A');
inline constexpr u32 kContainerVersion = 1;

// Builds a container in memory.
// Thread-safety: not thread safe; one writer per file.
class FOH_API ContainerWriter {
public:
    ContainerWriter(u32 typeId, u32 typeVersion, AssetId id);

    // `codec` is a request: the chunk is stored raw when compression does not shrink it.
    void addChunk(u32 tag, std::span<const u8> data, ChunkCodec codec = ChunkCodec::None);
    void addChunk(u32 tag, const std::vector<u8>& data, ChunkCodec codec = ChunkCodec::None) {
        addChunk(tag, std::span<const u8>(data), codec);
    }

    std::vector<u8> finish() const;

private:
    struct Pending {
        u32 tag;
        ChunkCodec codec;
        u64 rawSize;
        u64 hash;
        std::vector<u8> stored;
    };
    u32 m_typeId;
    u32 m_typeVersion;
    AssetId m_id;
    std::vector<Pending> m_chunks;
};

struct ChunkInfo {
    u32 tag = 0;
    ChunkCodec codec = ChunkCodec::None;
    u64 offset = 0;
    u64 storedSize = 0;
    u64 rawSize = 0;
    u64 hash = 0;
};

// Read access to a container held in memory.
// Thread-safety: const methods may be called concurrently.
class FOH_API ContainerReader {
public:
    // Validates the header and the chunk table. On failure returns nullopt and, if
    // given, fills `error`.
    static std::optional<ContainerReader> Open(std::vector<u8> file, std::string* error = nullptr);

    u32 typeId() const { return m_typeId; }
    u32 typeVersion() const { return m_typeVersion; }
    AssetId id() const { return m_id; }
    std::span<const ChunkInfo> chunks() const { return m_chunks; }
    const ChunkInfo* findChunk(u32 tag) const;

    // Decompresses a chunk and verifies its hash. False if missing or corrupt.
    bool readChunk(u32 tag, std::vector<u8>& out) const;

private:
    std::vector<u8> m_file;
    std::vector<ChunkInfo> m_chunks;
    u32 m_typeId = 0;
    u32 m_typeVersion = 0;
    AssetId m_id;
};

// Whole-file helpers (binary). WriteFileBytes creates parent directories and replaces
// the file atomically (temp file + rename), so a watcher never sees a half-written asset.
FOH_API bool ReadFileBytes(const std::filesystem::path& path, std::vector<u8>& out);
FOH_API bool WriteFileBytes(const std::filesystem::path& path, std::span<const u8> data);

} // namespace foh
