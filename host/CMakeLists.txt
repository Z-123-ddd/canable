cmake_minimum_required(VERSION 3.21)

project(canable2_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort)

# ---- libusb-1.0 (用于 DFU 固件更新) ----
# 默认装在 D:/libusb（可用 -DLIBUSB_ROOT=... 覆盖）
set(LIBUSB_ROOT "D:/libusb" CACHE PATH "libusb-1.0 root")
find_path(LIBUSB_INCLUDE_DIR libusb-1.0/libusb.h
    PATHS "${LIBUSB_ROOT}/include" NO_DEFAULT_PATH)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0
    PATHS "${LIBUSB_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR "未找到 libusb-1.0（期望 ${LIBUSB_ROOT}/include 与 /lib）")
endif()

add_executable(canable2_host WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/Protocol.cpp
    src/Link.h
    src/SerialLink.cpp
    src/UsbLink.cpp
    src/dfu_device.cpp
    src/FirmwareDialog.cpp
)

target_include_directories(canable2_host PRIVATE src ${LIBUSB_INCLUDE_DIR})

target_link_libraries(canable2_host PRIVATE
    Qt6::Widgets
    Qt6::SerialPort
    ${LIBUSB_LIBRARY}
)

# 把 libusb-1.0.dll 拷到可执行程序旁，便于直接运行
add_custom_command(TARGET canable2_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIBUSB_ROOT}/bin/libusb-1.0.dll"
        "$<TARGET_FILE_DIR:canable2_host>"
    COMMENT "Copy libusb-1.0.dll next to canable2_host.exe"
)
