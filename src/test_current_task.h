/**
 * @file test_current_task.h
 * @brief Header for Current Task BLE characteristic and History Service tests
 */

#ifndef TEST_CURRENT_TASK_H
#define TEST_CURRENT_TASK_H

/**
 * @brief Test History Service TLV encoding/decoding
 */
void test_history_service_tlv(void);

/**
 * @brief Test History Service control commands
 */
void test_history_service_commands(void);

/**
 * @brief Test History event recording
 */
void test_history_event_recording(void);

/**
 * @brief Test History Service settings
 */
void test_history_service_settings(void);

/**
 * @brief Test History Insights functionality
 */
void test_history_insights(void);

/**
 * @brief Test duration-based current task notification
 */
void test_current_task_notification(void);

/**
 * @brief Test ending current task notification
 */
void test_end_current_task(void);

/**
 * @brief Test volume-based current task notification
 */
void test_volume_based_task(void);

/**
 * @brief Run all History Service and current task tests
 */
void run_current_task_tests(void);

#endif /* TEST_CURRENT_TASK_H */
