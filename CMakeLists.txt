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

add_subdirectory(lib)
add_subdirectory(src)
