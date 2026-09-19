cmake_minimum_required(VERSION 3.20)
project(iforce_vigem_port LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_path(LIBUSB_INCLUDE_DIR NAMES libusb-1.0/libusb.h REQUIRED)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 REQUIRED)

find_path(VIGEM_INCLUDE_DIR NAMES ViGEmClient.h REQUIRED)
find_library(VIGEM_LIBRARY NAMES ViGEmClient ViGEmBus REQUIRED)

add_executable(iforce_vigem_port
    main.cpp
    iforce_protocol.cpp
    usb_device.cpp
    vigem_gamepad.cpp
)

target_include_directories(iforce_vigem_port PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUSB_INCLUDE_DIR}
    ${VIGEM_INCLUDE_DIR}
)

target_link_libraries(iforce_vigem_port PRIVATE
    ${LIBUSB_LIBRARY}
    ${VIGEM_LIBRARY}
)

if(WIN32)
    target_link_libraries(iforce_vigem_port PRIVATE setupapi)
endif()

install(TARGETS iforce_vigem_port RUNTIME DESTINATION bin)
