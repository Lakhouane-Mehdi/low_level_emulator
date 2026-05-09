#include "Input.hpp"

int Input::mapKey(sf::Keyboard::Key k) {
    using K = sf::Keyboard::Key;
    switch (k) {
    case K::Num1: return 0x1;
    case K::Num2: return 0x2;
    case K::Num3: return 0x3;
    case K::Num4: return 0xC;
    case K::Q:    return 0x4;
    case K::W:    return 0x5;
    case K::E:    return 0x6;
    case K::R:    return 0xD;
    case K::A:    return 0x7;
    case K::S:    return 0x8;
    case K::D:    return 0x9;
    case K::F:    return 0xE;
    case K::Z:    return 0xA;
    case K::X:    return 0x0;
    case K::C:    return 0xB;
    case K::V:    return 0xF;
    default:      return -1;
    }
}

bool Input::handleKeyPressed(const sf::Event::KeyPressed& kp, Chip8& cpu) {
    int k = mapKey(kp.code);
    if (k < 0) return false;
    cpu.keys[k] = 1;
    return true;
}

bool Input::handleKeyReleased(const sf::Event::KeyReleased& kr, Chip8& cpu) {
    int k = mapKey(kr.code);
    if (k < 0) return false;
    cpu.keys[k] = 0;
    return true;
}
