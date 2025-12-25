# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Board-specific overlay selection
if(BOARD STREQUAL "promicro_nrf52840")
    set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/promicro_52840.overlay)
elseif(BOARD STREQUAL "arduino_nano_33_ble")
    set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/arduino_nano_33_ble.overlay)
elseif(BOARD STREQUAL "native_sim")
    set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/native_sim.overlay)
endif()

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

# Generate enhanced database files from CSV
add_custom_command(
    OUTPUT 
        ${SRC_DIR}/plant_full_db.c
        ${SRC_DIR}/plant_full_db.inc
        ${SRC_DIR}/soil_enhanced_db.c
        ${SRC_DIR}/soil_enhanced_db.inc
        ${SRC_DIR}/irrigation_methods_db.c
        ${SRC_DIR}/irrigation_methods_db.inc
    COMMAND ${PYTHON_EXECUTABLE} build_database.py
    WORKING_DIRECTORY ${TOOLS_DIR}
    DEPENDS 
        ${TOOLS_DIR}/plants_full.csv
        ${TOOLS_DIR}/soil_db_new.csv
        ${TOOLS_DIR}/irrigation_methods.csv
        ${TOOLS_DIR}/build_database.py
        ${TOOLS_DIR}/csv2plant_full.py
        ${TOOLS_DIR}/csv2soil_enhanced.py
        ${TOOLS_DIR}/csv2irrigation_methods.py
    COMMENT "Generating irrigation database from CSV files..."
    VERBATIM
)

# Create a target for the generated files
add_custom_target(generate_database 
    DEPENDS 
        ${SRC_DIR}/plant_full_db.c
        ${SRC_DIR}/plant_full_db.inc
        ${SRC_DIR}/soil_enhanced_db.c
        ${SRC_DIR}/soil_enhanced_db.inc
        ${SRC_DIR}/irrigation_methods_db.c
        ${SRC_DIR}/irrigation_methods_db.inc
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(AutoWatering)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ── application sources ──────────────────────────────────────────────────────
target_sources(app PRIVATE
    # Core application
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
    src/timezone.c
    src/usb_descriptors.c
    
    # Storage and configuration
    src/nvs_config.c
    src/soil_moisture_config.c
    src/nvs_storage_monitor.c
    src/configuration_status.c
    src/onboarding_state.c
    src/reset_controller.c
    
    # BLE interface
    src/bt_irrigation_service.c
    src/bt_environmental_history_handlers.c
    src/bt_custom_soil_handlers.c
    src/bt_interval_mode_handlers.c
    
    # Environmental sensors
    src/env_sensors.c
    src/environmental_data.c
    src/environmental_history.c
    src/bme280_driver.c
    src/sensor_manager.c
    
    # Intelligence and calculations
    src/fao56_calc.c
    src/fao56_custom_soil.c
    src/custom_soil_db.c
    # Plant / soil / irrigation database public API (accessor functions)
    src/plant_db_api.c
    
    # Compensation systems
    src/rain_sensor.c
    src/rain_config.c
    src/rain_history.c
    src/rain_integration.c
    src/rain_compensation.c
    src/temperature_compensation.c
    src/temperature_compensation_integration.c
    
    # Interval mode
    src/interval_timing.c
    src/interval_mode_controller.c
    src/interval_task_integration.c
    
    # Enhanced system features
    src/enhanced_system_status.c
    src/enhanced_error_handling.c
    src/power_management.c
    
    # External flash storage (W25Q128 + LittleFS)
    src/database_flash.c
    src/history_flash.c
    
    # Generated enhanced database files - will be created automatically
    ${SRC_DIR}/plant_full_db.c
    ${SRC_DIR}/soil_enhanced_db.c
    ${SRC_DIR}/irrigation_methods_db.c
)

# Make sure database is generated before compiling app
add_dependencies(app generate_database)
