cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(astra-update)

# Detect platform and include corresponding CMake file
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(STATUS "Building on macOS")
    include(${CMAKE_SOURCE_DIR}/cmake/CMakeLists.macos.txt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Building on Linux")
    include(${CMAKE_SOURCE_DIR}/cmake/CMakeLists.linux.txt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(STATUS "Building on Windows")
    include(${CMAKE_SOURCE_DIR}/cmake/CMakeLists.windows.txt)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

include_directories(include)

include(CTest)
option(ASTRA_ENABLE_COVERAGE "Instrument project library and unit tests for gcov coverage" OFF)
if(ASTRA_ENABLE_COVERAGE AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "ASTRA_ENABLE_COVERAGE requires GCC and gcov")
endif()

add_subdirectory(lib)
add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(ASTRA_BUILD_TESTS "Build library event tests" OFF)
if(ASTRA_BUILD_TESTS)
    enable_testing()
    add_executable(astra-device-events-test tests/device_events.cpp)
    target_include_directories(astra-device-events-test PRIVATE lib ${CMAKE_BINARY_DIR}/yaml-cpp/include)
    target_link_libraries(astra-device-events-test PRIVATE astraupdate)
    add_test(NAME astra-device-events COMMAND astra-device-events-test)
endif()
