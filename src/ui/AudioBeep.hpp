#pragma once

#include <SFML/Audio.hpp>

#include <array>
#include <cstdint>

// Audio output for the emulator's sound timer.
//
//  - Classic CHIP-8 / SUPER-CHIP / MX-8: a 440Hz square-wave beep, looped
//    while the sound timer is hot. This is the historical behavior and is
//    used whenever no XO-CHIP audio pattern has been loaded.
//
//  - XO-CHIP: a 128-bit (16-byte) pattern buffer played back as 1-bit samples
//    at a pitch-controlled rate. The playback frequency follows the Octo
//    spec:  rate = 4000 * 2^((pitch - 64) / 48) Hz. setXoPattern() swaps the
//    looping buffer to the synthesized waveform; an all-zero pattern reverts
//    to silence (matching real XO-CHIP, where a zero buffer makes no sound).
//
// The buffer is only rebuilt when the (pattern, pitch) pair actually changes,
// so update() stays cheap to call every frame.
class AudioBeep {
public:
    static constexpr int PATTERN_BYTES = 16;

    AudioBeep();

    // Gate playback on the sound timer. Call once per frame.
    void update(uint8_t soundTimer, bool muted);

    // Install the XO-CHIP audio pattern + pitch. No-op if unchanged. Passing
    // an all-zero pattern selects silence. Switches subsequent playback away
    // from the classic square-wave beep for the rest of the session (until
    // another pattern is set), which is what XO-CHIP ROMs expect.
    void setXoPattern(const std::array<uint8_t, PATTERN_BYTES>& pattern, uint8_t pitch);

private:
    sf::SoundBuffer buffer_;
    sf::Sound       sound_;

    bool                               xo_active_ = false;
    std::array<uint8_t, PATTERN_BYTES> last_pattern_{};
    uint8_t                            last_pitch_ = 64;
    bool                               silent_     = false;  // all-zero XO pattern

    static sf::SoundBuffer makeSquareWaveBuffer(double freq, double duration, unsigned sampleRate);
    static sf::SoundBuffer makeXoPatternBuffer(const std::array<uint8_t, PATTERN_BYTES>& pattern,
                                               uint8_t pitch, unsigned sampleRate);
};
