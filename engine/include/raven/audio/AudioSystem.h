// RavenEngine — audio playback via miniaudio. Safe no-op when no output device
// exists (CI runners, headless servers): Initialize reports failure but every call stays valid.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <glm/glm.hpp>

namespace raven {

class AudioSystem {
public:
    struct Config {
        uint32_t maxSounds = 64;
    };

    AudioSystem() = default;
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool Initialize();
    bool Initialize(const Config& config);
    void Shutdown();
    bool IsAvailable() const;   // false when device init failed (safe no-op mode)

    // Loads a file into memory and registers it as a playable sound under `key`.
    bool LoadSound(const std::string& key, const std::filesystem::path& file);
    // Play by key. 3D sounds take a world position; others play flat.
    bool Play(const std::string& key, float volume = 1.0f, bool loop = false,
              const glm::vec3* position3D = nullptr);
    // Reaps finished sounds; call once per frame.
    void Update();
    void StopAll();

    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace raven
