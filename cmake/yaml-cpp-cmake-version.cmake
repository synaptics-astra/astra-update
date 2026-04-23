# Patch to update yaml-cpp CMakeLists.txt cmake_minimum_required syntax
# Fixes: CMake Deprecation Warning about compatibility with CMake < 3.10

set(CMAKELISTS_FILE "${SOURCE_DIR}/CMakeLists.txt")

if(NOT EXISTS "${CMAKELISTS_FILE}")
    message(FATAL_ERROR "Could not find ${CMAKELISTS_FILE}")
endif()

file(READ "${CMAKELISTS_FILE}" CMAKELISTS_CONTENT)

# Update cmake_minimum_required to use modern syntax with version range
string(REGEX REPLACE "cmake_minimum_required\\(VERSION 3\\.10\\)" 
                     "cmake_minimum_required(VERSION 3.10...3.30)" 
                     PATCHED_CONTENT "${CMAKELISTS_CONTENT}")

if(NOT PATCHED_CONTENT STREQUAL CMAKELISTS_CONTENT)
    file(WRITE "${CMAKELISTS_FILE}" "${PATCHED_CONTENT}")
    message(STATUS "Patched yaml-cpp CMakeLists.txt with modern cmake_minimum_required syntax")
else()
    message(STATUS "yaml-cpp CMakeLists.txt already uses modern syntax or pattern not found")
endif()
