cmake_minimum_required(VERSION 3.10)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_PATH} "/usr/lib/aarch64-linux-gnu/pkgconfig")
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")

project(wheel_legged_robot_tspi C CXX)

set(MQTT_DIR /opt/paho_mqtt_arm64)
set(LIBDRM_LIB /home/xiao/tspi_buildroot_system_build/Release/debian/ubuntu-build-service/buster-desktop-arm64/chroot/usr/lib/aarch64-linux-gnu/libdrm.so.2)

# ============================================================
# LVGL Configuration
# ============================================================
set(LV_CONF_PATH
    ${CMAKE_SOURCE_DIR}/library/lv_conf.h
    CACHE STRING "" FORCE)

# Collect LVGL core sources
file(GLOB_RECURSE LVGL_SOURCES ${CMAKE_SOURCE_DIR}/library/lvgl/src/*.c)

# Collect LVGL demo sources
file(GLOB_RECURSE LVGL_DEMO_SOURCES ${CMAKE_SOURCE_DIR}/library/lvgl/demos/*.c)

# Collect lv_drivers sources (DRM display + evdev input)
file(GLOB_RECURSE LV_DRIVERS_SOURCES ${CMAKE_SOURCE_DIR}/library/lv_drivers/*.c)

# Build LVGL as a static library
add_library(lvgl STATIC ${LVGL_SOURCES} ${LVGL_DEMO_SOURCES})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC
    ${CMAKE_SOURCE_DIR}/library             # lv_conf.h, lv_drv_conf.h
    ${CMAKE_SOURCE_DIR}/library/lvgl        # lvgl.h
    ${CMAKE_SOURCE_DIR}/library/lvgl/src    # lvgl internal headers
    ${CMAKE_SOURCE_DIR}/library/lvgl/demos  # demo headers
    /usr/include/libdrm                     # DRM headers (drm.h, drm_fourcc.h)
    /usr/include                            # xf86drm.h, xf86drmMode.h
)

# Build lv_drivers as a static library
add_library(lv_drivers STATIC ${LV_DRIVERS_SOURCES})
target_compile_definitions(lv_drivers PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
target_include_directories(lv_drivers PUBLIC
    ${CMAKE_SOURCE_DIR}/library             # lv_conf.h, lv_drv_conf.h
    ${CMAKE_SOURCE_DIR}/library/lv_drivers  # driver headers
    ${CMAKE_SOURCE_DIR}/library/lvgl        # lvgl.h
    /usr/include/libdrm
    /usr/include
)
target_link_libraries(lv_drivers PUBLIC lvgl)

# ============================================================
# Application Sources
# ============================================================

# BlueZ Bluetooth library (from Buildroot sysroot)
set(BUILDROOT_SYSROOT /home/xiao/tspi_buildroot_system_build/Release/buildroot/output/rockchip_rk3566/host/aarch64-buildroot-linux-gnu/sysroot)
set(BLUETOOTH_INC ${BUILDROOT_SYSROOT}/usr/include)
set(BLUETOOTH_LIB ${BUILDROOT_SYSROOT}/usr/lib/libbluetooth.so)

include_directories(
    ${CMAKE_SOURCE_DIR}/library/mqtt
    ${CMAKE_SOURCE_DIR}/devices/ap6212
    ${CMAKE_SOURCE_DIR}/library             # lv_conf.h, lv_drv_conf.h
    ${CMAKE_SOURCE_DIR}/library/lvgl        # lvgl.h
    ${CMAKE_SOURCE_DIR}/library/lvgl/demos  # demo headers
    ${CMAKE_SOURCE_DIR}/library/lv_drivers  # lv_drivers headers
    ${MQTT_DIR}/include
    ${BLUETOOTH_INC}
    /usr/include/libdrm
    /usr/include
)

link_directories(${MQTT_DIR}/lib)

# MQTT library sources
set(MQTT_SOURCES
    library/mqtt/lib_mqtt.c
)

# Device driver sources
set(DEVICE_SOURCES
    devices/ap6212/dev_ap6212_wifi.c
    devices/ap6212/dev_ap6212_bt.c
    devices/ap6212/dev_ap6212_ble_gatt.c
)

# GIF emoji resources
file(GLOB GIF_SOURCES ${CMAKE_SOURCE_DIR}/resources/gif/*.c)

set(MAIN_SOURCES main.c ${GIF_SOURCES})

add_library(mqtt_manager STATIC ${MQTT_SOURCES})
target_link_libraries(mqtt_manager paho-mqtt3c pthread)

add_library(device_drivers STATIC ${DEVICE_SOURCES})
target_include_directories(device_drivers PRIVATE ${BLUETOOTH_INC})

add_executable(wheel_legged_robot_tspi ${MAIN_SOURCES})
target_link_libraries(wheel_legged_robot_tspi
    mqtt_manager
    device_drivers
    lv_drivers
    lvgl
    ${LIBDRM_LIB}
    ${BLUETOOTH_LIB}
    pthread
    m
)