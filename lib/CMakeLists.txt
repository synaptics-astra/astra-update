include(ExternalProject)
include(FetchContent)

find_package(Threads REQUIRED)

set(LIBUSB_PLATFORM_CMAKE_ARGS)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND LIBUSB_PLATFORM_CMAKE_ARGS "-DHAVE_PIPE2:INTERNAL=")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(YAML_CPP_LIB_NAME "$<IF:$<CONFIG:Debug>,yaml-cppd,yaml-cpp>")
else()
    set(YAML_CPP_LIB_NAME "libyaml-cpp")
endif()

ExternalProject_Add(libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG v1.0.26
    PREFIX ${CMAKE_BINARY_DIR}/libusb
    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/libusb/lib/libusb-1.0${CMAKE_STATIC_LIBRARY_SUFFIX}
    CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF
               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
               -DCMAKE_POSITION_INDEPENDENT_CODE=ON
               -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/libusb
               -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
               -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
               -DCMAKE_C_FLAGS=-D_GNU_SOURCE
               ${LIBUSB_PLATFORM_CMAKE_ARGS}
)

ExternalProject_Add(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG yaml-cpp-0.7.0
    PREFIX ${CMAKE_BINARY_DIR}/yaml-cpp
    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/yaml-cpp/lib/${YAML_CPP_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}
    PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_BINARY_DIR}/yaml-cpp/src/yaml-cpp 
                                   -P ${CMAKE_SOURCE_DIR}/cmake/yaml-cpp-cstdint.cmake
    CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF
               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
               -DCMAKE_POSITION_INDEPENDENT_CODE=ON
               -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/yaml-cpp
               -DCMAKE_INSTALL_LIBDIR=lib
               -DYAML_CPP_BUILD_TESTS=OFF
               -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
               -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
               -DCMAKE_POLICY_VERSION_MINIMUM=3.10
)

file(GLOB SRC astra_boot_image.cpp
                astra_console.cpp
                astra_device.cpp
                astra_device_impl.cpp
                astra_device_impl_sl16xx.cpp
                astra_device_impl_sl26xx.cpp
                astra_log.cpp
                astra_device_manager.cpp
                boot_image_collection.cpp
                emmc_flash_image.cpp
                fastboot_device.cpp
                flash_image.cpp
                image.cpp
                libusb_device.cpp
                libusb_transport.cpp
                nand_flash_image.cpp
                spi_flash_image.cpp
                usb_cdc_device.cpp
                usb_device.cpp
                usb_cdc_transport.cpp
                usb_transport.cpp
                utils.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND SRC win_libusb_transport.cpp win_usb_cdc_transport.cpp win_usb_cdc_device.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND SRC posix_usb_cdc_device.cpp posix_usb_cdc_transport.cpp posix_usb_cdc_transport_macos.cpp)
else()
    list(APPEND SRC posix_usb_cdc_device.cpp posix_usb_cdc_transport.cpp posix_usb_cdc_transport_linux.cpp)
endif()

add_library(astraupdate STATIC ${SRC})
set_target_properties(astraupdate PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_dependencies(astraupdate yaml-cpp)
add_dependencies(astraupdate libusb)

message(CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME})

target_include_directories(astraupdate PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/include)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/libusb/include)


target_link_libraries(astraupdate PRIVATE Threads::Threads ${CMAKE_BINARY_DIR}/libusb/lib/libusb-1.0${CMAKE_STATIC_LIBRARY_SUFFIX} ${PLATFORM_LINK_LIBRARIES})
target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/lib/${YAML_CPP_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})

if(ASTRA_ENABLE_COVERAGE)
    target_compile_options(astraupdate PRIVATE --coverage -O0 -g)
    target_link_libraries(astraupdate INTERFACE --coverage)
endif()

# Python bindings with pybind11
if(ASTRA_BUILD_PYTHON_BINDINGS)
if(CMAKE_VERSION VERSION_LESS "3.18")
    message(FATAL_ERROR "Python bindings require CMake 3.18 or newer")
endif()
FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v3.0.2
)
set(PYBIND11_FINDPYTHON ON)
find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
FetchContent_MakeAvailable(pybind11)

# Build Python extension module (_astra_update follows the common
# underscore-prefixed naming convention for native extension modules).
pybind11_add_module(_astra_update astra_update_py.cpp)

# Link the static library to the Python module
target_link_libraries(_astra_update PRIVATE astraupdate)

# Add dependency to ensure astraupdate is built first
add_dependencies(_astra_update astraupdate)

# Include directories needed for the Python module
target_include_directories(_astra_update PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(_astra_update PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(_astra_update PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/include)
target_include_directories(_astra_update PRIVATE ${CMAKE_BINARY_DIR}/libusb/include)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/python/astra_update/
    DESTINATION astra_update
    FILES_MATCHING
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.py"
)
install(TARGETS _astra_update
    LIBRARY DESTINATION astra_update
    RUNTIME DESTINATION astra_update
)

if(NOT SKBUILD)
    # Keep direct CMake builds importable from the source tree.
    add_custom_command(TARGET _astra_update POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:_astra_update>
            ${CMAKE_SOURCE_DIR}/python/astra_update/$<TARGET_FILE_NAME:_astra_update>
    )
endif()
endif()
