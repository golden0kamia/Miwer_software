cmake_minimum_required(VERSION 3.16)
project(miwer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_executable(miwer
    src/main.cpp
    src/config.cpp
    src/serial_device.cpp
    src/pipewire_mixer.cpp
)

target_compile_options(miwer PRIVATE -Wall -Wextra)
target_link_libraries(miwer PRIVATE PkgConfig::PIPEWIRE)

install(TARGETS miwer RUNTIME DESTINATION bin)
