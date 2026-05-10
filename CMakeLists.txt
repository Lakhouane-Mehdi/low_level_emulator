cmake_minimum_required(VERSION 3.16)
project(CHIP8_Emulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# SFML 3.x via vcpkg.
find_package(SFML 3 COMPONENTS Graphics Window System Audio CONFIG REQUIRED)

add_executable(chip8
    src/main.cpp
    src/core/Chip8.cpp
    src/core/Memory.cpp
    src/core/Disassembler.cpp
    src/core/isa/IInstructionSet.cpp
    src/core/isa/Chip8ISA.cpp
    src/core/isa/SchipISA.cpp
    src/core/isa/Mx8ISA.cpp
    src/ui/Renderer.cpp
    src/ui/Input.cpp
    src/ui/AudioBeep.cpp
    src/ui/DebugView.cpp
    src/ui/RomBrowser.cpp
    src/ui/FontLoader.cpp
    src/app/App.cpp
)

target_include_directories(chip8 PRIVATE src)
target_link_libraries(chip8 PRIVATE
    SFML::Graphics
    SFML::Window
    SFML::System
    SFML::Audio
)

# Copy roms + assets next to the binary so relative paths work for both
# `./chip8` and Visual Studio's working-directory-is-project-root layout.
add_custom_command(TARGET chip8 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/roms $<TARGET_FILE_DIR:chip8>/roms)

add_custom_command(TARGET chip8 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:chip8>/assets/fonts)

if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
    add_custom_command(TARGET chip8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:chip8>/assets)
endif()

# Common headless core sources shared by chip8_test and chip8_run.
set(CHIP8_CORE_SOURCES
    src/core/Chip8.cpp
    src/core/Memory.cpp
    src/core/Disassembler.cpp
    src/core/isa/IInstructionSet.cpp
    src/core/isa/Chip8ISA.cpp
    src/core/isa/SchipISA.cpp
    src/core/isa/Mx8ISA.cpp
)

# Headless core test runner — no SFML, links only against src/core/.
# Run via: cmake --build build --config Release --target chip8_test
#         build/Release/chip8_test.exe
add_executable(chip8_test tests/core_test.cpp ${CHIP8_CORE_SOURCES})
target_include_directories(chip8_test PRIVATE src)
enable_testing()
add_test(NAME core COMMAND chip8_test)

# Headless ROM runner — for CI / fuzzing / regression suites.
# Run via: build/Release/chip8_run.exe rom.ch8 --frames N [...]
add_executable(chip8_run tools/chip8_run.cpp ${CHIP8_CORE_SOURCES})
target_include_directories(chip8_run PRIVATE src)
