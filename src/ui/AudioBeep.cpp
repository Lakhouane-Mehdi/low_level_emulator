#include "AudioBeep.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

sf::SoundBuffer AudioBeep::makeSquareWaveBuffer(double freq, double duration, unsigned sampleRate) {
    std::vector<std::int16_t> samples(static_cast<size_t>(sampleRate * duration));
    const double TWO_PI = 6.28318530717958647692;
    for (size_t i = 0; i < samples.size(); ++i) {
        double t = static_cast<double>(i) / sampleRate;
        samples[i] = (std::sin(TWO_PI * freq * t) > 0 ? 6000 : -6000);
    }
    sf::SoundBuffer buf;
    if (!buf.loadFromSamples(samples.data(), samples.size(), 1, sampleRate,
                             {sf::SoundChannel::Mono})) {
        std::fprintf(stderr, "AudioBeep: failed to load sample buffer\n");
    }
    return buf;
}

// XO-CHIP audio: the 16-byte pattern is 128 1-bit samples (MSB first). They
// are emitted at a playback rate of 4000 * 2^((pitch-64)/48) Hz. We render
// one period of the pattern, upsampled to the host sample rate, and loop it.
sf::SoundBuffer AudioBeep::makeXoPatternBuffer(
        const std::array<uint8_t, PATTERN_BYTES>& pattern, uint8_t pitch, unsigned sampleRate) {
    // Octo's playback-rate formula.
    const double playbackRate = 4000.0 * std::pow(2.0, (static_cast<double>(pitch) - 64.0) / 48.0);
    const int    bitCount     = PATTERN_BYTES * 8;   // 128 1-bit samples

    // Host samples per XO sample = sampleRate / playbackRate. One full pattern
    // period therefore spans (bitCount * sampleRate / playbackRate) host
    // samples. Guard against absurd rates.
    const double samplesPerBit = static_cast<double>(sampleRate) / playbackRate;
    const size_t total = static_cast<size_t>(static_cast<double>(bitCount) * samplesPerBit);

    std::vector<std::int16_t> samples(total ? total : 1);
    for (size_t i = 0; i < samples.size(); ++i) {
        // Which of the 128 pattern bits does this host sample fall on?
        int bit = static_cast<int>(static_cast<double>(i) / samplesPerBit);
        if (bit >= bitCount) bit = bitCount - 1;
        const uint8_t byte = pattern[bit / 8];
        const bool    on   = (byte >> (7 - (bit % 8))) & 1;
        samples[i] = on ? 6000 : -6000;
    }

    sf::SoundBuffer buf;
    if (!buf.loadFromSamples(samples.data(), samples.size(), 1, sampleRate,
                             {sf::SoundChannel::Mono})) {
        std::fprintf(stderr, "AudioBeep: failed to load XO pattern buffer\n");
    }
    return buf;
}

AudioBeep::AudioBeep()
    : buffer_(makeSquareWaveBuffer(440.0, 0.05, 44100)),
      sound_(buffer_) {
    sound_.setLooping(true);
}

void AudioBeep::setXoPattern(const std::array<uint8_t, PATTERN_BYTES>& pattern, uint8_t pitch) {
    // Skip the rebuild if nothing changed — update() runs every frame.
    if (xo_active_ && pattern == last_pattern_ && pitch == last_pitch_) return;

    last_pattern_ = pattern;
    last_pitch_   = pitch;
    xo_active_    = true;

    bool all_zero = true;
    for (uint8_t b : pattern) if (b) { all_zero = false; break; }
    silent_ = all_zero;
    if (all_zero) {
        // Real XO-CHIP makes no sound for an empty pattern; reflect that by
        // marking silence and stopping any current playback.
        if (sound_.getStatus() == sf::Sound::Status::Playing) sound_.stop();
        return;
    }

    const bool was_playing = sound_.getStatus() == sf::Sound::Status::Playing;
    sound_.stop();
    buffer_ = makeXoPatternBuffer(pattern, pitch, 44100);
    sound_.setBuffer(buffer_);
    sound_.setLooping(true);
    if (was_playing) sound_.play();
}

void AudioBeep::update(uint8_t soundTimer, bool muted) {
    const bool shouldPlay = soundTimer > 0 && !muted && !(xo_active_ && silent_);
    const bool playing    = sound_.getStatus() == sf::Sound::Status::Playing;
    if (shouldPlay && !playing) sound_.play();
    if (!shouldPlay && playing) sound_.stop();
}
