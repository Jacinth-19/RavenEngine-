#include "raven/audio/AudioSystem.h"

#include <miniaudio.h>

#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdio>

#include "raven/core/Logger.h"

namespace raven {

struct AudioSystem::Impl {
    ma_engine engine{};
    bool engineReady = false;

    struct SoundEntry {
        std::vector<unsigned char> bytes;   // decoded on each Play()
    };
    struct PlayingEntry {
        std::unique_ptr<ma_sound> sound;
        std::unique_ptr<ma_decoder> decoder;
    };

    std::unordered_map<std::string, SoundEntry> sounds;
    std::vector<PlayingEntry> playing;
};

AudioSystem::~AudioSystem() {
    Shutdown();
}

bool AudioSystem::Initialize() { return Initialize(Config{}); }

bool AudioSystem::Initialize(const Config& config) {
    if (m_Impl && m_Impl->engineReady) return true;
    m_Impl = std::make_unique<Impl>();

    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.listenerCount = 1;
    engineConfig.channels = 2;
    ma_result result = ma_engine_init(&engineConfig, &m_Impl->engine);
    if (result != MA_SUCCESS) {
        RAVEN_LOG_WARN("AudioSystem: no audio device available (ma_result=%d) — continuing silent", static_cast<int>(result));
        m_Impl->engineReady = false;
        return false;
    }
    m_Impl->engineReady = true;
    RAVEN_LOG_INFO("AudioSystem: initialized (max %u sounds)", config.maxSounds);
    return true;
}

void AudioSystem::Shutdown() {
    if (!m_Impl) return;
    for (auto& p : m_Impl->playing) {
        if (p.sound) ma_sound_uninit(p.sound.get());
        if (p.decoder) ma_decoder_uninit(p.decoder.get());
    }
    m_Impl->playing.clear();
    m_Impl->sounds.clear();
    if (m_Impl->engineReady) ma_engine_uninit(&m_Impl->engine);
    m_Impl.reset();
}

bool AudioSystem::IsAvailable() const {
    return m_Impl && m_Impl->engineReady;
}

bool AudioSystem::LoadSound(const std::string& key, const std::filesystem::path& file) {
    if (!IsAvailable()) return false;
    if (m_Impl->sounds.find(key) != m_Impl->sounds.end()) return true;

    FILE* fp = std::fopen(file.string().c_str(), "rb");
    if (!fp) {
        RAVEN_LOG_ERROR("AudioSystem: cannot open %s", file.string().c_str());
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    Impl::SoundEntry entry;
    if (size > 0) {
        entry.bytes.resize(static_cast<size_t>(size));
        size_t read = std::fread(entry.bytes.data(), 1, entry.bytes.size(), fp);
        entry.bytes.resize(read);
    }
    std::fclose(fp);
    m_Impl->sounds.emplace(key, std::move(entry));
    return true;
}

bool AudioSystem::Play(const std::string& key, float volume, bool loop, const glm::vec3* position3D) {
    if (!IsAvailable()) return false;
    auto it = m_Impl->sounds.find(key);
    if (it == m_Impl->sounds.end()) {
        RAVEN_LOG_WARN("AudioSystem: unknown sound '%s'", key.c_str());
        return false;
    }

    Update();  // reap finished sounds first

    // Decode from memory: decoder owns the decode state; the sound holds a reference to it.
    // We keep both alive in PlayingEntry until the sound finishes.
    auto decoder = std::make_unique<ma_decoder>();
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result decoderResult = ma_decoder_init_memory(it->second.bytes.data(), it->second.bytes.size(), &decoderConfig, decoder.get());
    if (decoderResult != MA_SUCCESS) {
        RAVEN_LOG_ERROR("AudioSystem: decode failed for '%s' (ma_result=%d)", key.c_str(), static_cast<int>(decoderResult));
        return false;
    }
    auto soundOwner = std::make_unique<ma_sound>();
    ma_result result = ma_sound_init_from_data_source(&m_Impl->engine, decoder.get(), MA_SOUND_FLAG_DECODE, nullptr, soundOwner.get());
    if (result != MA_SUCCESS) {
        RAVEN_LOG_ERROR("AudioSystem: sound init failed for '%s' (ma_result=%d)", key.c_str(), static_cast<int>(result));
        ma_decoder_uninit(decoder.get());
        return false;
    }
    ma_sound_set_volume(soundOwner.get(), volume);
    if (loop) ma_sound_set_looping(soundOwner.get(), MA_TRUE);
    if (position3D) {
        ma_sound_set_spatialization_enabled(soundOwner.get(), MA_TRUE);
        ma_sound_set_position(soundOwner.get(), position3D->x, position3D->y, position3D->z);
        ma_sound_set_min_distance(soundOwner.get(), 1.0f);
        ma_sound_set_max_distance(soundOwner.get(), 30.0f);
        ma_sound_set_rolloff(soundOwner.get(), 1.0f);
    } else {
        ma_sound_set_spatialization_enabled(soundOwner.get(), MA_FALSE);
    }
    ma_sound_start(soundOwner.get());

    m_Impl->playing.push_back(Impl::PlayingEntry{std::move(soundOwner), std::move(decoder)});
    return true;
}

void AudioSystem::Update() {
    if (!m_Impl) return;
    auto& playing = m_Impl->playing;
    for (auto it = playing.begin(); it != playing.end();) {
        ma_sound* s = it->sound.get();
        if (s && ma_sound_at_end(s)) {
            ma_sound_uninit(s);
            if (it->decoder) ma_decoder_uninit(it->decoder.get());
            it = playing.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::StopAll() {
    if (!IsAvailable()) return;
    for (auto& p : m_Impl->playing) {
        if (p.sound) {
            ma_sound_stop(p.sound.get());
            ma_sound_uninit(p.sound.get());
        }
        if (p.decoder) ma_decoder_uninit(p.decoder.get());
    }
    m_Impl->playing.clear();
}

void AudioSystem::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    if (!IsAvailable()) return;
    ma_engine_listener_set_position(&m_Impl->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&m_Impl->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&m_Impl->engine, 0, up.x, up.y, up.z);
}

} // namespace raven
