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

AudioBeep::AudioBeep()
    : buffer_(makeSquareWaveBuffer(440.0, 0.05, 44100)),
      sound_(buffer_) {
    sound_.setLooping(true);
}

void AudioBeep::update(uint8_t soundTimer, bool muted) {
    const bool shouldPlay = soundTimer > 0 && !muted;
    const bool playing    = sound_.getStatus() == sf::Sound::Status::Playing;
    if (shouldPlay && !playing) sound_.play();
    if (!shouldPlay && playing) sound_.stop();
}
