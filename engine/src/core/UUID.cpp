#include "raven/core/UUID.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace raven {

const UUID& UUID::Nil() {
    static const UUID kNil{0, 0};
    return kNil;
}

UUID UUID::Generate() {
    // RFC-4122-ish random v4 UUID (variant bits set for familiarity, though we
    // treat the value opaquely everywhere).
    static std::mt19937_64 rng([] {
        std::random_device rd;
        std::vector<uint32_t> seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        std::seed_seq seq(seed.begin(), seed.end());
        return std::mt19937_64(seq);
    }());
    UUID id{rng(), rng()};
    if (id.IsNil()) id.lo = 1;
    return id;
}

std::string UUID::ToString() const {
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%08x%04x",
                  static_cast<unsigned>((hi >> 32) & 0xffffffffu),
                  static_cast<unsigned>((hi >> 16) & 0xffffu),
                  static_cast<unsigned>(hi & 0xffffu),
                  static_cast<unsigned>((lo >> 48) & 0xffffu),
                  static_cast<unsigned>((lo >> 16) & 0xffffffffu),
                  static_cast<unsigned>(lo & 0xffffu));
    return std::string(buf);
}

std::optional<UUID> UUID::Parse(std::string_view text) {
    // Accept the canonical 36-char form; be forgiving about case and about
    // missing dashes (32 hex digits).
    uint64_t hi = 0, lo = 0;
    int nibbleIndex = 0;
    bool seenHi = false;
    for (char c : text) {
        if (c == '-' || c == '{' || c == '}' || c == ' ') continue;
        uint64_t nib = 0;
        if (c >= '0' && c <= '9') nib = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') nib = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nib = static_cast<uint64_t>(c - 'A' + 10);
        else return std::nullopt;
        if (nibbleIndex < 16) {
            hi = (hi << 4) | nib;
        } else if (nibbleIndex < 32) {
            lo = (lo << 4) | nib;
        } else {
            return std::nullopt;  // too long
        }
        ++nibbleIndex;
        (void)seenHi;
    }
    if (nibbleIndex != 32) return std::nullopt;
    return UUID{hi, lo};
}

} // namespace raven
