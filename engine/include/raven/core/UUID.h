// RavenEngine — 128-bit UUID used for assets and editor objects.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <optional>

namespace raven {

class UUID {
public:
    uint64_t hi = 0;
    uint64_t lo = 0;

    constexpr UUID() = default;
    constexpr UUID(uint64_t hi_, uint64_t lo_) : hi(hi_), lo(lo_) {}

    static const UUID& Nil();
    static UUID Generate();
    static std::optional<UUID> Parse(std::string_view text);

    std::string ToString() const;  // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    bool IsNil() const { return hi == 0 && lo == 0; }

    constexpr bool operator==(const UUID& o) const { return hi == o.hi && lo == o.lo; }
    constexpr bool operator!=(const UUID& o) const { return !(*this == o); }
    bool operator<(const UUID& o) const { return hi == o.hi ? lo < o.lo : hi < o.hi; }
};

struct UUIDHash {
    size_t operator()(const UUID& id) const {
        // Simple, decent avalanche (splitmix64 finalizer on both halves).
        uint64_t a = id.hi + 0x9e3779b97f4a7c15ULL;
        a = (a ^ (a >> 30)) * 0xbf58476d1ce4e5b9ULL;
        a = (a ^ (a >> 27)) * 0x94d049bb133111ebULL;
        a ^= (a >> 31);
        uint64_t b = id.lo + 0x9e3779b97f4a7c15ULL;
        b = (b ^ (b >> 30)) * 0xbf58476d1ce4e5b9ULL;
        b = (b ^ (b >> 27)) * 0x94d049bb133111ebULL;
        b ^= (b >> 31);
        return static_cast<size_t>(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
    }
};

} // namespace raven
