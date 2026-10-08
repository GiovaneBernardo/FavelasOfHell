#pragma once

// Stable identity of an asset or sub-asset (128 bit GUID). Lives in core so that
// components can reference assets without including the asset system. Plain data:
// trivially copyable, safe inside ECS components.

#include "foh/types.h"

namespace foh {

struct AssetId {
    u64 hi = 0;
    u64 lo = 0;
    bool operator==(const AssetId&) const = default;
    bool isValid() const { return (hi | lo) != 0; }
};

struct AssetIdHash {
    size_t operator()(const AssetId& id) const { return static_cast<size_t>(id.hi ^ (id.lo * 0x9E3779B97F4A7C15ull)); }
};
inline bool operator<(const AssetId& a, const AssetId& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }

} // namespace foh
