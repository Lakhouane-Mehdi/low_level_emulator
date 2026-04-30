cmake_minimum_required(VERSION 3.16)
project(CHIP8_Emulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# SFML 3.x via vcpkg: components are Graphics, Window, System, Audio (capitalized).
find_package(SFML 3 COMPONENTS Graphics Window System Audio CONFIG REQUIRED)

add_executable(chip8
    src/main.cpp
    src/Chip8.cpp
    src/Disassembler.cpp
)

target_include_directories(chip8 PRIVATE src)
target_link_libraries(chip8 PRIVATE
    SFML::Graphics
    SFML::Window
    SFML::System
    SFML::Audio
)

# Copy roms folder next to the binary so relative paths work
add_custom_command(TARGET chip8 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/roms $<TARGET_FILE_DIR:chip8>/roms
)
