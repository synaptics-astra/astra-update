# Patch to add #include <cstdint> to yaml-cpp/src/emitterutils.cpp
# This is needed for GCC 15+ which is stricter about includes
set(EMITTERUTILS_FILE "${SOURCE_DIR}/src/emitterutils.cpp")

if(NOT EXISTS "${EMITTERUTILS_FILE}")
    message(FATAL_ERROR "Could not find ${EMITTERUTILS_FILE}")
endif()

file(READ "${EMITTERUTILS_FILE}" EMITTERUTILS_CONTENT)

# Check if cstdint is already included
if(NOT EMITTERUTILS_CONTENT MATCHES "#include <cstdint>")
    # Find where the includes section ends and insert our include there
    string(REGEX REPLACE "(#include \"yaml-cpp/null\\.h\"\n#include \"yaml-cpp/ostream_wrapper\\.h\")" 
                         "#include <cstdint>\n\\1" 
                         PATCHED_CONTENT "${EMITTERUTILS_CONTENT}")
    
    if(NOT PATCHED_CONTENT STREQUAL EMITTERUTILS_CONTENT)
        file(WRITE "${EMITTERUTILS_FILE}" "${PATCHED_CONTENT}")
        message(STATUS "Patched yaml-cpp emitterutils.cpp with #include <cstdint>")
    else()
        message(WARNING "Failed to patch yaml-cpp emitterutils.cpp")
    endif()
endif()
