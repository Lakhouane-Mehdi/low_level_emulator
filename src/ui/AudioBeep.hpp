#pragma once

#include <SFML/Audio.hpp>

// 440Hz square-wave beep, played in a loop while the CHIP-8 sound timer is hot.
// Owns the SoundBuffer so we can keep `Sound` alive for the whole session.
class AudioBeep {
public:
    AudioBeep();

    void update(uint8_t soundTimer, bool muted);

private:
    sf::SoundBuffer buffer_;
    sf::Sound       sound_;
    static sf::SoundBuffer makeSquareWaveBuffer(double freq, double duration, unsigned sampleRate);
};
