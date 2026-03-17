include(ExternalProject)
include(FetchContent)

ExternalProject_Add(libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG v1.0.26
    PREFIX ${CMAKE_BINARY_DIR}/libusb
    CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF
               -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/libusb
               -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
               -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
)

ExternalProject_Add(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG yaml-cpp-0.7.0
    PREFIX ${CMAKE_BINARY_DIR}/yaml-cpp
    CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF
               -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/yaml-cpp
               -DYAML_CPP_BUILD_TESTS=OFF
               -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
               -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
)

ExternalProject_Add(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    PREFIX ${CMAKE_BINARY_DIR}/zlib
    CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF
               -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/zlib
               -DCMAKE_POSITION_INDEPENDENT_CODE=ON
               -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
               -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
)

file(GLOB SRC astra_boot_image.cpp
                astra_console.cpp
                astra_device.cpp
                astra_device_impl_sl16xx.cpp
                astra_device_impl_sl26xx.cpp
                astra_log.cpp
                astra_device_manager.cpp
                boot_image_collection.cpp
                emmc_flash_image.cpp
                flash_image.cpp
                image.cpp
                libusb_device.cpp
                libusb_transport.cpp
                spi_flash_image.cpp
                usb_cdc_device.cpp
                usb_device.cpp
                usb_cdc_transport.cpp
                usb_transport.cpp
                utils.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND SRC win_libusb_transport.cpp win_usb_cdc_transport.cpp win_usb_cdc_device.cpp)
else()
    list(APPEND SRC posix_usb_cdc_device.cpp posix_usb_cdc_transport.cpp)
endif()

add_library(astraupdate STATIC ${SRC})

add_dependencies(astraupdate yaml-cpp)
add_dependencies(astraupdate libusb)
add_dependencies(astraupdate zlib)

message(CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(YAML_CPP_LIB_NAME "yaml-cppd")
        set(ZLIB_LIB_NAME "zlibstaticd")
    else()
        set(YAML_CPP_LIB_NAME "yaml-cpp")
        set(ZLIB_LIB_NAME "zlibstatic")
    endif()
else()
    set(YAML_CPP_LIB_NAME "libyaml-cpp")
    set(ZLIB_LIB_NAME "libz")
endif()

target_include_directories(astraupdate PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/include)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/libusb/include)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/zlib/include)


target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/libusb/lib/libusb-1.0${CMAKE_STATIC_LIBRARY_SUFFIX} ${PLATFORM_LINK_LIBRARIES})
target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/lib/${YAML_CPP_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})
target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/zlib/lib/${ZLIB_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})
