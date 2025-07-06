/**
 * @file test_current_task.c
 * @brief Test file for Current Task BLE characteristic and History Service
 * 
 * This file provides examples of how to use the new Current Task
 * characteristic for real-time monitoring of watering tasks and
 * demonstrates the new History Service functionality.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "watering.h"
#include "bt_irrigation_service.h"
#include "watering_history.h"

#ifdef CONFIG_BT

/**
 * @brief Test the new History Service TLV functionality
 */
void test_history_service_tlv(void) {
    printk("Testing History Service TLV encoding/decoding...\n");
    
    uint8_t buffer[20];
    int result;
    
    // Test uint8 encoding
    result = tlv_encode_uint8(buffer, HT_CHANNEL_ID, 3);
    if (result > 0) {
        printk("TLV uint8 encoded: %d bytes\n", result);
        
        uint8_t decoded_value;
        int decode_result = tlv_decode_uint8(buffer, HT_CHANNEL_ID, &decoded_value);
        if (decode_result > 0 && decoded_value == 3) {
            printk("TLV uint8 decode successful: %d\n", decoded_value);
        } else {
            printk("TLV uint8 decode failed\n");
        }
    }
    
    // Test uint32 encoding
    result = tlv_encode_uint32(buffer, HT_RANGE_START, 1735927200);
    if (result > 0) {
        printk("TLV uint32 encoded: %d bytes\n", result);
        
        uint32_t decoded_value;
        int decode_result = tlv_decode_uint32(buffer, HT_RANGE_START, &decoded_value);
        if (decode_result > 0 && decoded_value == 1735927200) {
            printk("TLV uint32 decode successful: %u\n", decoded_value);
        } else {
            printk("TLV uint32 decode failed\n");
        }
    }
}

/**
 * @brief Test History Service control commands
 */
void test_history_service_commands(void) {
    printk("Testing History Service control commands...\n");
    
    uint8_t ctrl_buffer[20];
    int offset = 0;
    
    // Build a QUERY_RANGE command
    ctrl_buffer[offset++] = HC_QUERY_RANGE;
    
    // Add channel ID TLV
    offset += tlv_encode_uint8(&ctrl_buffer[offset], HT_CHANNEL_ID, 1);
    
    // Add start time TLV
    offset += tlv_encode_uint32(&ctrl_buffer[offset], HT_RANGE_START, 1735927200);
    
    // Add end time TLV
    offset += tlv_encode_uint32(&ctrl_buffer[offset], HT_RANGE_END, 1735930800);
    
    // Process the command
    watering_error_t err = history_ctrl_handler(ctrl_buffer, offset);
    if (err == WATERING_SUCCESS) {
        printk("History control command processed successfully\n");
    } else {
        printk("History control command failed: %d\n", err);
    }
}

/**
 * @brief Test adding events to the new History system
 */
void test_history_event_recording(void) {
    printk("Testing History event recording...\n");
    
    // Test recording a task start
    watering_error_t err = watering_history_record_task_start(
        2,                          // channel 2
        WATERING_MODE_VOLUME,       // volume-based
        1500,                       // 1.5 liters
        WATERING_TRIGGER_MANUAL     // manual trigger
    );
    
    if (err == WATERING_SUCCESS) {
        printk("Task start recorded successfully\n");
    } else {
        printk("Task start recording failed: %d\n", err);
    }
    
    k_sleep(K_SECONDS(1));
    
    // Test recording a task completion
    err = watering_history_record_task_complete(
        2,                          // channel 2
        1450,                       // 1.45 liters actual
        1450,                       // 1.45 liters total volume
        WATERING_SUCCESS_COMPLETE   // completed successfully
    );
    
    if (err == WATERING_SUCCESS) {
        printk("Task completion recorded successfully\n");
    } else {
        printk("Task completion recording failed: %d\n", err);
    }
}

/**
 * @brief Test History Service settings
 */
void test_history_service_settings(void) {
    printk("Testing History Service settings...\n");
    
    history_settings_t settings;
    
    // Get current settings
    watering_error_t err = history_settings_get(&settings);
    if (err == WATERING_SUCCESS) {
        printk("Current settings: detailed=%d, daily=%d, monthly=%d, annual=%d\n",
               settings.detailed_cnt, settings.daily_days, 
               settings.monthly_months, settings.annual_years);
    } else {
        printk("Failed to get history settings: %d\n", err);
    }
    
    // Test updating settings
    history_settings_t new_settings = {
        .detailed_cnt = 25,
        .daily_days = 60,
        .monthly_months = 24,
        .annual_years = 5
    };
    
    err = history_settings_set(&new_settings);
    if (err == WATERING_SUCCESS) {
        printk("History settings updated successfully\n");
    } else {
        printk("Failed to update history settings: %d\n", err);
    }
}

/**
 * @brief Test Insights functionality
 */
void test_history_insights(void) {
    printk("Testing History Insights...\n");
    
    // Create sample insights data
    insights_t insights = {
        .weekly_ml = {1200, 1500, 800, 2000, 1100, 900, 1300, 1600},
        .leak = {0, 1, 0, 0, 0, 2, 0, 0},
        .efficiency_pct = 87
    };
    
    watering_error_t err = history_insights_update(&insights);
    if (err == WATERING_SUCCESS) {
        printk("Insights updated successfully\n");
        printk("Weekly volumes: %u, %u, %u, %u, %u, %u, %u, %u ml\n",
               insights.weekly_ml[0], insights.weekly_ml[1], insights.weekly_ml[2], insights.weekly_ml[3],
               insights.weekly_ml[4], insights.weekly_ml[5], insights.weekly_ml[6], insights.weekly_ml[7]);
        printk("Leak indicators: %d, %d, %d, %d, %d, %d, %d, %d\n",
               insights.leak[0], insights.leak[1], insights.leak[2], insights.leak[3],
               insights.leak[4], insights.leak[5], insights.leak[6], insights.leak[7]);
        printk("Overall efficiency: %d%%\n", insights.efficiency_pct);
    } else {
        printk("Failed to update insights: %d\n", err);
    }
}

/**
 * @brief Example function to demonstrate Current Task monitoring
 * 
 * This function shows how to manually trigger a current task update
 * for testing purposes.
 */
void test_current_task_notification(void) {
    printk("Testing Current Task BLE notification...\n");
    
    // Example: Simulate an active task on channel 0
    // Duration-based watering: 5 minutes (300 seconds)
    // 2 minutes elapsed (120 seconds)
    // 150ml total volume dispensed
    
    int result = bt_irrigation_current_task_update(
        0,      // channel_id: Channel 0
        1735927200, // start_time: Example Unix timestamp (2025-01-03 12:00:00)
        0,      // mode: 0 = duration-based watering
        300,    // target_value: 300 seconds (5 minutes)
        120,    // current_value: 120 seconds elapsed
        150     // total_volume: 150ml dispensed
    );
    
    if (result == 0) {
        printk("Current task notification sent successfully\n");
    } else {
        printk("Failed to send current task notification: %d\n", result);
    }
}

/**
 * @brief Example function to demonstrate ending a task
 * 
 * This function shows how to notify clients that no task is active.
 */
void test_end_current_task(void) {
    printk("Testing Current Task end notification...\n");
    
    // Notify that no task is active
    int result = bt_irrigation_current_task_update(
        0xFF,   // channel_id: 0xFF means no active task
        0,      // start_time: 0 (not applicable)
        0,      // mode: 0 (not applicable)
        0,      // target_value: 0 (not applicable)
        0,      // current_value: 0 (not applicable)
        0       // total_volume: 0 (not applicable)
    );
    
    if (result == 0) {
        printk("Task end notification sent successfully\n");
    } else {
        printk("Failed to send task end notification: %d\n", result);
    }
}

/**
 * @brief Example function to demonstrate volume-based task monitoring
 * 
 * This function shows how to monitor a volume-based watering task.
 */
void test_volume_based_task(void) {
    printk("Testing Volume-based Current Task notification...\n");
    
    // Example: Volume-based watering
    // Target: 2 liters (2000ml)
    // Current: 750ml dispensed
    // Total volume: 750ml
    
    int result = bt_irrigation_current_task_update(
        2,      // channel_id: Channel 2
        1735927800, // start_time: Example Unix timestamp (2025-01-03 12:10:00)
        1,      // mode: 1 = volume-based watering
        2000,   // target_value: 2000ml (2 liters)
        750,    // current_value: 750ml dispensed
        750     // total_volume: 750ml total
    );
    
    if (result == 0) {
        printk("Volume-based task notification sent successfully\n");
    } else {
        printk("Failed to send volume-based task notification: %d\n", result);
    }
}

/**
 * @brief Test function to run all History Service and Current Task examples
 * 
 * This function runs all the test examples in sequence.
 */
void run_current_task_tests(void) {
    printk("=== Starting History Service and Current Task BLE Tests ===\n");
    
    // Test 1: History Service TLV functionality
    test_history_service_tlv();
    k_sleep(K_SECONDS(1));
    
    // Test 2: History Service control commands
    test_history_service_commands();
    k_sleep(K_SECONDS(1));
    
    // Test 3: History event recording
    test_history_event_recording();
    k_sleep(K_SECONDS(1));
    
    // Test 4: History settings
    test_history_service_settings();
    k_sleep(K_SECONDS(1));
    
    // Test 5: History insights
    test_history_insights();
    k_sleep(K_SECONDS(1));
    
    // Test 6: Duration-based task
    test_current_task_notification();
    k_sleep(K_SECONDS(2));
    
    // Test 7: Volume-based task
    test_volume_based_task();
    k_sleep(K_SECONDS(2));
    
    // Test 8: End task
    test_end_current_task();
    
    printk("=== History Service and Current Task BLE Tests Complete ===\n");
}

#else

void test_history_service_tlv(void) {
    printk("Bluetooth not enabled - History Service tests skipped\n");
}

void test_history_service_commands(void) {
    printk("Bluetooth not enabled - History Service tests skipped\n");
}

void test_history_event_recording(void) {
    printk("Bluetooth not enabled - History Service tests skipped\n");
}

void test_history_service_settings(void) {
    printk("Bluetooth not enabled - History Service tests skipped\n");
}

void test_history_insights(void) {
    printk("Bluetooth not enabled - History Service tests skipped\n");
}

void test_current_task_notification(void) {
    printk("Bluetooth not enabled - Current Task tests skipped\n");
}

void test_end_current_task(void) {
    printk("Bluetooth not enabled - Current Task tests skipped\n");
}

void test_volume_based_task(void) {
    printk("Bluetooth not enabled - Current Task tests skipped\n");
}

void run_current_task_tests(void) {
    printk("Bluetooth not enabled - Current Task tests skipped\n");
}

#endif /* CONFIG_BT */
