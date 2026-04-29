# Patch to add #include <cstdint> to cxxopts/include/cxxopts.hpp
# Needed for GCC 15 where uint8_t is not implicitly visible.

set(CXXOPTS_HEADER_FILE "${SOURCE_DIR}/include/cxxopts.hpp")

if(NOT EXISTS "${CXXOPTS_HEADER_FILE}")
    message(FATAL_ERROR "Could not find ${CXXOPTS_HEADER_FILE}")
endif()

file(READ "${CXXOPTS_HEADER_FILE}" CXXOPTS_HEADER_CONTENT)

if(NOT CXXOPTS_HEADER_CONTENT MATCHES "#include <cstdint>")
    string(REGEX REPLACE "(#[ \t]*include <optional>[\r]?\n)"
                         "\\1#include <cstdint>\n"
                         PATCHED_CONTENT
                         "${CXXOPTS_HEADER_CONTENT}")

    if(NOT PATCHED_CONTENT STREQUAL CXXOPTS_HEADER_CONTENT)
        file(WRITE "${CXXOPTS_HEADER_FILE}" "${PATCHED_CONTENT}")
        message(STATUS "Patched cxxopts.hpp with #include <cstdint>")
    else()
        message(WARNING "Failed to patch cxxopts.hpp for cstdint include")
    endif()
endif()