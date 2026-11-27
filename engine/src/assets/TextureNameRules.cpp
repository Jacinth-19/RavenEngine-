#include "raven/assets/TextureNameRules.h"

#include <algorithm>
#include <cctype>

namespace raven {

namespace {

// Aliases per slot, most-specific first. Compared lowercased against the stem.
const std::vector<const char*> kAliases[static_cast<size_t>(TextureSlot::kCount)] = {
    /* Diffuse   */ {"diffuse", "albedo", "basecolor", "base_color", "diff", "dif", "d"},
    /* Normal    */ {"normal", "nrm", "normalmap", "bump", "bumpmap", "n"},
    /* Roughness */ {"roughness", "rough", "rgh", "r"},
    /* Metalness */ {"metalness", "metallic", "metal", "mtl", "m"},
    /* Emissive  */ {"emissive", "emit", "glow", "e"},
    /* Occlusion */ {"occlusion", "ao", "ambientocclusion"},
    /* Height    */ {"height", "disp", "displacement", "h"},
};

std::string ToLower(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool IsBoundary(char c) { return c == '_' || c == '-' || c == '.' || c == ' ' || c == '(' || c == ')'; }

// Positions at which `token` starts inside `hay` (case-insensitive).
std::vector<size_t> FindTokenOccurrences(const std::string& hay, const std::string& token) {
    std::vector<size_t> out;
    if (token.empty() || token.size() > hay.size()) return out;
    size_t pos = 0;
    while ((pos = hay.find(token, pos)) != std::string::npos) {
        const bool leftOk = pos == 0 || IsBoundary(hay[pos - 1]);
        const size_t end = pos + token.size();
        const bool rightOk = end == hay.size() || IsBoundary(hay[end]);
        out.push_back(leftOk && rightOk ? pos | (size_t{1} << 62) : pos);   // tag full-word matches
        pos = end;
    }
    return out;
}

} // namespace

const char* SlotToString(TextureSlot slot) {
    switch (slot) {
        case TextureSlot::Diffuse:   return "diffuse";
        case TextureSlot::Normal:    return "normal";
        case TextureSlot::Roughness: return "roughness";
        case TextureSlot::Metalness: return "metalness";
        case TextureSlot::Emissive:  return "emissive";
        case TextureSlot::Occlusion: return "occlusion";
        case TextureSlot::Height:    return "height";
        default:                     return "unknown";
    }
}

TextureSlot SlotFromString(const std::string& text) {
    const std::string lower = ToLower(text);
    for (size_t i = 0; i < static_cast<size_t>(TextureSlot::kCount); ++i)
        if (lower == SlotToString(static_cast<TextureSlot>(i))) return static_cast<TextureSlot>(i);
    return TextureSlot::kCount;
}

TextureSlot SlotFromToken(const std::string& text) {
    const std::string lower = ToLower(text);
    for (size_t i = 0; i < static_cast<size_t>(TextureSlot::kCount); ++i) {
        for (const char* alias : kAliases[i])
            if (lower == alias) return static_cast<TextureSlot>(i);
    }
    return TextureSlot::kCount;
}

int ScoreTextureForSlot(const std::string& stem, TextureSlot slot) {
    if (slot == TextureSlot::kCount) return 0;
    const size_t slotIndex = static_cast<size_t>(slot);
    const std::string lower = ToLower(stem);

    int best = 0;
    // Competing slots whose trailing token the stem carries — a strong anti-signal for this slot.
    int competing = 0;

    for (size_t i = 0; i < static_cast<size_t>(TextureSlot::kCount); ++i) {
        const bool isOwn = i == slotIndex;
        for (const char* alias : kAliases[i]) {
            const std::string token(alias);
            const size_t tokenLen = token.size();
            for (const size_t tagged : FindTokenOccurrences(lower, token)) {
                const bool wholeWord = (tagged & (size_t{1} << 62)) != 0;
                const size_t pos = tagged & ~(size_t{1} << 62);
                // A single-letter alias only counts at a word boundary — as a raw
                // substring it matches nearly every stem ("barn_wood" vs metal "m").
                if (!wholeWord && token.size() == 1) continue;
                const bool trailing = (pos + tokenLen == lower.size());
                int score = 0;
                if (trailing && wholeWord) score = 100 + static_cast<int>(tokenLen);
                else if (wholeWord)        score = 80 + static_cast<int>(tokenLen);
                else                       score = 40;
                if (isOwn) best = std::max(best, score);
                else       competing = std::max(competing, score);
            }
        }
    }

    if (best == 0) return 0;
    if (competing > best) return -competing;   // clearly belongs to a different slot
    return best;
}

std::vector<ScoredCandidate> RankCandidates(TextureSlot slot, const std::vector<std::string>& stems) {
    std::vector<ScoredCandidate> out;
    out.reserve(stems.size());
    for (const std::string& stem : stems) {
        const int score = ScoreTextureForSlot(stem, slot);
        if (score > 0) out.push_back({stem, score});
    }
    std::stable_sort(out.begin(), out.end(), [](const ScoredCandidate& a, const ScoredCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.stem < b.stem;
    });
    return out;
}

bool IsAmbiguous(const std::vector<ScoredCandidate>& ranked, int margin) {
    if (ranked.size() < 2) return false;
    return ranked[0].score - ranked[1].score <= margin;
}

} // namespace raven
