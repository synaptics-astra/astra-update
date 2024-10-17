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

file(GLOB SRC astra_boot_image.cpp
                astra_console.cpp
                astra_device.cpp
                astra_log.cpp
                astra_device_manager.cpp
                boot_image_collection.cpp
                emmc_flash_image.cpp
                flash_image.cpp
                image.cpp
                spi_flash_image.cpp
                usb_device.cpp
                usb_transport.cpp
                utils.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND SRC win_usb_transport.cpp)
endif()

add_library(astraupdate STATIC ${SRC})

add_dependencies(astraupdate yaml-cpp)
add_dependencies(astraupdate libusb)

message(CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(YAML_CPP_LIB_NAME "yaml-cppd")
    else()
        set(YAML_CPP_LIB_NAME "yaml-cpp")
    endif()
else()
    set(YAML_CPP_LIB_NAME "libyaml-cpp")
endif()

target_include_directories(astraupdate PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/include)
target_include_directories(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/libusb/include)


target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/libusb/lib/libusb-1.0${CMAKE_STATIC_LIBRARY_SUFFIX} ${PLATFORM_LINK_LIBRARIES})
target_link_libraries(astraupdate PRIVATE ${CMAKE_BINARY_DIR}/yaml-cpp/lib/${YAML_CPP_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})
