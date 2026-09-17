cmake_minimum_required(VERSION 3.21)

project(canable2_host VERSION 30.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

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

# ---- SOEM (EtherCAT 主站, v2.0.0, Win32/WinPcap) —— 静态库 ----
set(SOEM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/soem")
add_library(soem STATIC
    "${SOEM_DIR}/src/ec_base.c"
    "${SOEM_DIR}/src/ec_coe.c"
    "${SOEM_DIR}/src/ec_config.c"
    "${SOEM_DIR}/src/ec_dc.c"
    "${SOEM_DIR}/src/ec_eoe.c"
    "${SOEM_DIR}/src/ec_foe.c"
    "${SOEM_DIR}/src/ec_main.c"
    "${SOEM_DIR}/src/ec_print.c"
    "${SOEM_DIR}/src/ec_soe.c"
    "${SOEM_DIR}/osal/win32/osal.c"
    "${SOEM_DIR}/oshw/win32/oshw.c"
    "${SOEM_DIR}/oshw/win32/nicdrv.c"
    "${SOEM_DIR}/oshw/win32/wpcap_dynload.c"   # 运行时动态加载 wpcap.dll
)
target_include_directories(soem PUBLIC
    "${SOEM_DIR}/include"                 # soem/ec_*.h + 生成的 ec_options.h
    "${SOEM_DIR}/osal"
    "${SOEM_DIR}/osal/win32"
    "${SOEM_DIR}/oshw/win32"
    "${SOEM_DIR}/oshw/win32/wpcap/Include"
)
# MinGW(GNU) 用 _UCRT（与 SOEM Windows.cmake 一致）
target_compile_options(soem PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare>)
# 不链接 wpcap/Packet 导入库：改由 wpcap_dynload.c 运行时加载 wpcap.dll
# （否则无 Npcap 的机器连 exe 都启动不了）。只链接 Windows 系统库。
target_link_libraries(soem PUBLIC ws2_32 winmm)

add_executable(canable2_host WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/Protocol.cpp
    src/Link.h
    src/SerialLink.cpp
    src/UsbLink.cpp
    src/VendorDfuFlasher.cpp
    src/dfu_device.cpp
    src/FirmwareDialog.cpp
    src/TraceImporter.cpp
    src/SimpleProto.cpp
    src/CanOpenProto.cpp
    src/ObjectDictionaryDialog.cpp
    src/EtherCatLink.cpp
    resources.qrc
)

# 生成的 exe 文件名（品牌化）；内部 target 名仍为 canable2_host
set_target_properties(canable2_host PROPERTIES OUTPUT_NAME "CyberBeast-CANable2")

target_include_directories(canable2_host PRIVATE src ${LIBUSB_INCLUDE_DIR})

# 应用版本号（供「帮助→关于」显示；与 package.sh 的 APPVER 保持一致）
target_compile_definitions(canable2_host PRIVATE
    APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(canable2_host PRIVATE
    Qt6::Widgets
    Qt6::SerialPort
    ${LIBUSB_LIBRARY}
    soem
)

# 把 libusb-1.0.dll 拷到可执行程序旁，便于直接运行
add_custom_command(TARGET canable2_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIBUSB_ROOT}/bin/libusb-1.0.dll"
        "$<TARGET_FILE_DIR:canable2_host>"
    COMMENT "Copy libusb-1.0.dll next to CyberBeast-CANable2.exe"
)

# 厂商 DFU 运行时（32 位 helper + 两个 DLL）拷到 exe 旁的 vendor_dfu/
add_custom_command(TARGET canable2_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor_dfu"
        "$<TARGET_FILE_DIR:canable2_host>/vendor_dfu"
    COMMENT "Copy vendor_dfu runtime (nsi_dfu_flash.exe + NsMcuCmdDll.dll) next to CyberBeast-CANable2.exe"
)
