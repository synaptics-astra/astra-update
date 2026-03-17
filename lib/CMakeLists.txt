include(ExternalProject)
include(FetchContent)

set(LIBUSB_PLATFORM_CMAKE_ARGS)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND LIBUSB_PLATFORM_CMAKE_ARGS "-DHAVE_PIPE2:INTERNAL=")
endif()

ExternalProject_Add(libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG v1.0.26
    PREFIX ${CMAKE_BINARY_DIR}/libusb
    CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF
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
    PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_BINARY_DIR}/yaml-cpp/src/yaml-cpp 
                                   -P ${CMAKE_SOURCE_DIR}/cmake/yaml-cpp-cstdint.cmake
    CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF
               -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/yaml-cpp
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

add_dependencies(astraupdate yaml-cpp)
add_dependencies(astraupdate libusb)

message(CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(YAML_CPP_LIB_NAME "$<IF:$<CONFIG:Debug>,yaml-cppd,yaml-cpp>")
else()
    set(YAML_CPP_LIB_NAME "libyaml-cpp")
endif()

target_include_directories(astraupdate PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/include)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/libusb/include)


target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/libusb/lib/libusb-1.0${CMAKE_STATIC_LIBRARY_SUFFIX} ${PLATFORM_LINK_LIBRARIES})
target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/lib/${YAML_CPP_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})

if(ASTRA_ENABLE_COVERAGE)
    target_compile_options(astraupdate PRIVATE --coverage -O0 -g)
    target_link_libraries(astraupdate INTERFACE --coverage)
endif()

# Python bindings with pybind11
FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
)
FetchContent_MakeAvailable(pybind11)

# Find Python
find_package(Python REQUIRED COMPONENTS Interpreter Development)

# Build Python module
pybind11_add_module(astra_update astra_update_py.cpp)

# Link the static library to the Python module
target_link_libraries(astra_update PRIVATE astraupdate)

# Add dependency to ensure astraupdate is built first
add_dependencies(astra_update astraupdate)

# Include directories needed for the Python module
target_include_directories(astra_update PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(astra_update PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(astra_update PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/include)
target_include_directories(astra_update PRIVATE ${CMAKE_BINARY_DIR}/libusb/include)
target_include_directories(astra_update PRIVATE ${CMAKE_BINARY_DIR}/zlib/include)
