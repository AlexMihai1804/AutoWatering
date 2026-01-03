#ifndef RESET_CONTROLLER_H
#define RESET_CONTROLLER_H

/**
 * @file reset_controller.h
 * @brief Reset controller for AutoWatering system onboarding
 * 
 * This module provides comprehensive reset capabilities for different
 * system components with confirmation code validation for safety.
 */

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Reset operation types
 */
typedef enum {
    RESET_TYPE_CHANNEL_CONFIG,      /**< Reset single channel configuration */
    RESET_TYPE_CHANNEL_SCHEDULE,    /**< Reset single channel schedule */
    RESET_TYPE_ALL_CHANNELS,        /**< Reset all channel configurations */
    RESET_TYPE_ALL_SCHEDULES,       /**< Reset all schedules */
    RESET_TYPE_SYSTEM_CONFIG,       /**< Reset system configuration */
    RESET_TYPE_CALIBRATION,         /**< Reset calibration data */
    RESET_TYPE_HISTORY,             /**< Clear history data */
    RESET_TYPE_FACTORY_RESET        /**< Complete factory reset */
} reset_type_t;

/**
 * @brief Reset request structure
 */
typedef struct {
    reset_type_t type;              /**< Type of reset to perform */
    uint8_t channel_id;             /**< Channel ID for channel-specific resets (0-7) */
    uint32_t confirmation_code;     /**< Safety confirmation code */
    uint8_t reserved[4];            /**< Reserved for future use */
} __attribute__((packed)) reset_request_t;

/**
 * @brief Reset operation status codes
 */
typedef enum {
    RESET_STATUS_SUCCESS = 0,       /**< Reset completed successfully */
    RESET_STATUS_INVALID_TYPE,      /**< Invalid reset type */
    RESET_STATUS_INVALID_CHANNEL,   /**< Invalid channel ID */
    RESET_STATUS_INVALID_CODE,      /**< Invalid confirmation code */
    RESET_STATUS_CODE_EXPIRED,      /**< Confirmation code expired */
    RESET_STATUS_STORAGE_ERROR,     /**< NVS storage error */
    RESET_STATUS_SYSTEM_ERROR       /**< General system error */
} reset_status_t;

/**
 * @brief Reset confirmation code structure
 */
typedef struct {
    uint32_t code;                  /**< Generated confirmation code */
    reset_type_t type;              /**< Reset type this code is valid for */
    uint8_t channel_id;             /**< Channel ID (if applicable) */
    uint32_t generation_time;       /**< When code was generated (timestamp) */
    uint32_t expiry_time;           /**< When code expires (timestamp) */
    bool is_valid;                  /**< Whether code is currently valid */
    uint8_t reserved[3];            /**< Reserved for future use */
} __attribute__((packed)) reset_confirmation_t;

/* Confirmation code validity period (5 minutes) */
#define RESET_CONFIRMATION_VALIDITY_SEC  300

/* ========================================================================= */
/*                         PERSISTENT WIPE STATE MACHINE                     */
/* ========================================================================= */

/**
 * @brief Wipe state machine states (persisted to NVS)
 * These match BLE status byte encoding.
 */
typedef enum {
    WIPE_STATE_IDLE        = 0x00,  /**< No wipe in progress */
    WIPE_STATE_AWAIT_CONFIRM = 0x01, /**< Waiting for confirmation code */
    WIPE_STATE_IN_PROGRESS = 0x02,  /**< Wipe executing (step-by-step) */
    WIPE_STATE_DONE_OK     = 0x03,  /**< Wipe completed successfully */
    WIPE_STATE_DONE_ERROR  = 0x04   /**< Wipe completed with error */
} wipe_state_t;

/**
 * @brief Wipe execution steps (persisted to NVS for resume on reboot)
 */
typedef enum {
    WIPE_STEP_PREPARE         = 0,   /**< Initialize wipe, persist state */
    WIPE_STEP_RESET_CHANNELS  = 1,   /**< Reset all 8 channel configs */
    WIPE_STEP_RESET_SYSTEM    = 2,   /**< Reset system configuration */
    WIPE_STEP_RESET_CALIBRATION = 3, /**< Reset calibration data */
    WIPE_STEP_CLEAR_RAIN_HIST = 4,   /**< Clear rain history (flash erase) */
    WIPE_STEP_CLEAR_ENV_HIST  = 5,   /**< Clear environmental history */
    WIPE_STEP_CLEAR_ONBOARDING = 6,  /**< Clear onboarding NVS flags */
    WIPE_STEP_VERIFY          = 7,   /**< Verify all data erased */
    WIPE_STEP_DONE            = 8,   /**< Cleanup and finalize */
    WIPE_STEP_COUNT           = 9    /**< Total number of steps */
} wipe_step_t;

/**
 * @brief Persistent wipe state (saved to NVS after each step)
 */
typedef struct {
    wipe_state_t state;              /**< Current wipe state */
    wipe_step_t current_step;        /**< Current or next step to execute */
    uint8_t attempt_count;           /**< Retry counter for current step */
    uint16_t last_error;             /**< Last error code encountered */
    uint32_t started_uptime_ms;      /**< When wipe started (for timeout) */
    uint8_t reserved[3];             /**< Reserved for future use */
} __attribute__((packed)) wipe_progress_t;

/* Maximum retries per step before failing */
#define WIPE_MAX_STEP_RETRIES  3

/* Timeout for entire wipe operation (30 seconds) */
#define WIPE_TIMEOUT_MS  30000

/**
 * @brief Get percentage progress for a given step
 * @param step Current wipe step
 * @return Progress percentage (0-100)
 */
uint8_t wipe_step_to_progress_pct(wipe_step_t step);

/**
 * @brief Get current wipe progress
 * @param progress Pointer to store progress info
 * @return 0 on success, negative error code on failure
 */
int reset_controller_get_wipe_progress(wipe_progress_t *progress);

/**
 * @brief Resume wipe if one was in progress (call at boot)
 * @return 0 if no resume needed, 1 if resumed, negative on error
 */
int reset_controller_resume_wipe(void);

/**
 * @brief Start a new factory wipe (async, will send BLE notifications)
 * @param confirmation_code Validated confirmation code
 * @return 0 on success, negative error code on failure
 */
int reset_controller_start_factory_wipe(uint32_t confirmation_code);

/**
 * @brief Execute next step of wipe (called from work queue)
 * @return 0 if step completed, 1 if wipe done, negative on error
 */
int reset_controller_execute_wipe_step(void);

/**
 * @brief Abort wipe and set error state
 * @param error Error code to record
 */
void reset_controller_abort_wipe(uint16_t error);

/**
 * @brief Clear wipe state (after app acknowledges DONE_OK/DONE_ERROR)
 * @return 0 on success, negative on error
 */
int reset_controller_clear_wipe_state(void);

/**
 * @brief Initialize the reset controller system
 * 
 * @return 0 on success, negative error code on failure
 */
int reset_controller_init(void);

/**
 * @brief Generate a confirmation code for a reset operation
 * 
 * @param type Type of reset operation
 * @param channel_id Channel ID (for channel-specific resets, ignored otherwise)
 * @return Generated confirmation code, or 0 on error
 */
uint32_t reset_controller_generate_confirmation_code(reset_type_t type, uint8_t channel_id);

/**
 * @brief Execute a reset operation with confirmation code validation
 * 
 * @param request Reset request with confirmation code
 * @return reset_status_t indicating the result of the operation
 */
reset_status_t reset_controller_execute(const reset_request_t *request);

/**
 * @brief Validate a confirmation code
 * 
 * @param code Confirmation code to validate
 * @param type Expected reset type
 * @param channel_id Expected channel ID (for channel-specific resets)
 * @return True if code is valid, false otherwise
 */
bool reset_controller_validate_confirmation_code(uint32_t code, reset_type_t type, uint8_t channel_id);

/**
 * @brief Get the current confirmation code information
 * 
 * @param confirmation Pointer to store confirmation information
 * @return 0 on success, negative error code on failure
 */
int reset_controller_get_confirmation_info(reset_confirmation_t *confirmation);

/**
 * @brief Clear/invalidate the current confirmation code
 * 
 * @return 0 on success, negative error code on failure
 */
int reset_controller_clear_confirmation_code(void);

/**
 * @brief Check if a reset type requires a channel ID
 * 
 * @param type Reset type to check
 * @return True if channel ID is required, false otherwise
 */
bool reset_controller_requires_channel_id(reset_type_t type);

/**
 * @brief Get a human-readable description of a reset type
 * 
 * @param type Reset type
 * @return String description of the reset type
 */
const char* reset_controller_get_type_description(reset_type_t type);

/**
 * @brief Get a human-readable description of a reset status
 * 
 * @param status Reset status code
 * @return String description of the status
 */
const char* reset_controller_get_status_description(reset_status_t status);

#endif /* RESET_CONTROLLER_H */