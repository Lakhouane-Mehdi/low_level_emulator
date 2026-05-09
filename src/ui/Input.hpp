#pragma once

#include "../core/Chip8.hpp"

#include <SFML/Window/Event.hpp>
#include <SFML/Window/Keyboard.hpp>

// Translates SFML keyboard events into CHIP-8 keypad state.
//   1 2 3 C        1 2 3 4
//   4 5 6 D   <->  Q W E R
//   7 8 9 E        A S D F
//   A 0 B F        Z X C V
class Input {
public:
    // Returns the CHIP-8 nibble (0..F) for a physical key, or -1 if not mapped.
    static int mapKey(sf::Keyboard::Key k);

    // Updates cpu.keys[] for press/release events. Returns true if it consumed
    // the event (i.e. a CHIP-8 key); false otherwise so the App can dispatch it.
    static bool handleKeyPressed(const sf::Event::KeyPressed& kp, Chip8& cpu);
    static bool handleKeyReleased(const sf::Event::KeyReleased& kr, Chip8& cpu);
};
