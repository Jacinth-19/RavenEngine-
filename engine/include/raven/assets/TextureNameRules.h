// RavenEngine — texture name-rule scoring for smart model import (EDITOR_DESIGN §10.3).
//
// Pure string logic over file-name stems: given a texture slot (diffuse, normal, …)
// rank a list of texture stems by how well each matches the slot. Importer uses the
// ranking for all-confident matching; the editor's TextureRemapDialog shows it for
// ambiguous slots. Case-insensitive throughout; token matches at a word boundary
// ('_', '-', '.', or string edges) score highest.
#pragma once

#include <string>
#include <vector>

namespace raven {

enum class TextureSlot : uint8_t {
    Diffuse = 0,    // albedo / basecolor / baseColor / _d
    Normal,         // normal / nrm / _n / bump
    Roughness,      // roughness / _rough / _r
    Metalness,      // metallic / metalness / _metal / _m
    Emissive,       // emissive / _emissive / glow / _e
    Occlusion,      // occlusion / ao / _ao
    Height,         // height / disp / displacement / _height
    kCount
};

const char* SlotToString(TextureSlot slot);                 // "diffuse", "normal", …
TextureSlot SlotFromString(const std::string& text);        // kCount when unknown
TextureSlot SlotFromToken(const std::string& text);         // scans alias tables (e.g. "_n")

// Score `stem` against `slot`. 0 = no signal; larger is better.
//   100 + token length — exact trailing token ("crate_n" vs normal)
//    80 + token length — token at any word boundary
//    40                — token found as a substring
//   Negative when the stem is a *different* slot's trailing token (suppresses false picks).
int ScoreTextureForSlot(const std::string& stem, TextureSlot slot);

struct ScoredCandidate {
    std::string stem;
    int score = 0;
};

// Ranks `stems` for `slot`, dropping score-0 entries. Ties break by stem (stable).
std::vector<ScoredCandidate> RankCandidates(TextureSlot slot, const std::vector<std::string>& stems);

// True when the top two candidates are within `margin` points of each other
// (and neither is clearly dominant) — the importer reports instead of guessing.
bool IsAmbiguous(const std::vector<ScoredCandidate>& ranked, int margin = 15);

} // namespace raven
