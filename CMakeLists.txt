# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(AutoWatering)

target_sources(app PRIVATE
  src/main.c
  src/flow_sensor.c
  src/watering.c
  src/watering_tasks.c
  src/watering_config.c
  src/watering_monitor.c
)
