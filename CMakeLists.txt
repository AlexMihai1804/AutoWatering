# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Set the correct overlay file path - fix the filename
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/promicro_52840.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(AutoWatering)

target_sources(app PRIVATE
  src/main.c
  src/flow_sensor.c
  src/watering.c
  src/watering_tasks.c
  src/watering_config.c
  src/watering_monitor.c
  src/rtc.c
  src/valve_control.c
  src/watering_log.c
  src/bt_irrigation_service.c
)

# Ensure the include directories are available
target_include_directories(app PRIVATE src)
