cmake_minimum_required(VERSION 3.16)
project(miwer VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(MIWER_BUILD_TESTS "Compiler les tests unitaires (CTest)" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# Protocole série : sans dépendance à PipeWire, partagé avec les tests
add_library(miwer_protocol STATIC src/device_protocol.cpp)
target_include_directories(miwer_protocol PUBLIC src)
target_compile_options(miwer_protocol PRIVATE -Wall -Wextra)

add_executable(miwer
    src/main.cpp
    src/config.cpp
    src/serial_device.cpp
    src/pipewire_mixer.cpp
)

target_compile_options(miwer PRIVATE -Wall -Wextra)
target_link_libraries(miwer PRIVATE miwer_protocol PkgConfig::PIPEWIRE)

install(TARGETS miwer RUNTIME DESTINATION bin)

if(MIWER_BUILD_TESTS)
    enable_testing()
    add_executable(test_protocol tests/test_protocol.cpp src/config.cpp)
    target_compile_options(test_protocol PRIVATE -Wall -Wextra)
    target_link_libraries(test_protocol PRIVATE miwer_protocol)
    add_test(NAME protocol COMMAND test_protocol)

    # Bout en bout avec le simulateur : nécessite python3 et une session PipeWire
    # (aucun volume réel n'est modifié). « ctest -LE e2e » pour l'ignorer.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME e2e_sim COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_sim.sh $<TARGET_FILE:miwer>)
        set_tests_properties(e2e_sim PROPERTIES LABELS e2e TIMEOUT 60)
    endif()
endif()
