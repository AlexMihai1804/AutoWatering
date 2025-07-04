# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Board-specific overlay
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/promicro_52840.overlay)

# Auto-generate database files from CSV before build
set(TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Check if Python and tools exist
find_program(PYTHON_EXECUTABLE 
    NAMES python python3 python.exe python3.exe
    PATHS ENV PATH
    DOC "Python interpreter for database generation"
)

# Try using West's Python if system Python not found
if(NOT PYTHON_EXECUTABLE AND DEFINED WEST_PYTHON)
    set(PYTHON_EXECUTABLE ${WEST_PYTHON})
    message(STATUS "Using West Python: ${PYTHON_EXECUTABLE}")
endif()

if(NOT PYTHON_EXECUTABLE)
    message(FATAL_ERROR "Python not found! Required for database generation. WEST_PYTHON=${WEST_PYTHON}")
endif()

message(STATUS "Using Python: ${PYTHON_EXECUTABLE}")

# Generate database files from CSV
add_custom_command(
    OUTPUT 
        ${SRC_DIR}/soil_table.c
        ${SRC_DIR}/soil_table.inc
        ${SRC_DIR}/plant_db.c
        ${SRC_DIR}/plant_db.inc
    COMMAND ${PYTHON_EXECUTABLE} build_database.py
    WORKING_DIRECTORY ${TOOLS_DIR}
    DEPENDS 
        ${TOOLS_DIR}/soil_table.csv
        ${TOOLS_DIR}/plant_db.csv
        ${TOOLS_DIR}/build_database.py
        ${TOOLS_DIR}/validate_db.py
        ${TOOLS_DIR}/csv2soil_header.py
        ${TOOLS_DIR}/csv2plant_header.py
    COMMENT "Generating irrigation database from CSV files..."
    VERBATIM
)

# Create a target for the generated files
add_custom_target(generate_database 
    DEPENDS 
        ${SRC_DIR}/soil_table.c
        ${SRC_DIR}/soil_table.inc
        ${SRC_DIR}/plant_db.c
        ${SRC_DIR}/plant_db.inc
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(AutoWatering)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ── application sources ──────────────────────────────────────────────────────
target_sources(app PRIVATE
    src/main.c
    src/valve_control.c
    src/flow_sensor.c
    src/watering.c
    src/watering_tasks.c
    src/watering_monitor.c
    src/watering_log.c
    src/watering_config.c
    src/watering_history.c
    src/rtc.c
    src/usb_descriptors.c
    src/nvs_config.c
    src/bt_irrigation_service.c
    # Generated database files - will be created automatically
    ${SRC_DIR}/soil_table.c
    ${SRC_DIR}/plant_db.c
)

# Make sure database is generated before compiling app
add_dependencies(app generate_database)
