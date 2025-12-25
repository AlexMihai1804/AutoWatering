#include "interval_task_integration.h"
#include "watering.h"
#include "flow_sensor.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>

LOG_MODULE_REGISTER(interval_task_integration, LOG_LEVEL_DBG);

/* External references */
extern watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];
extern struct watering_task_state_t watering_task_state;

/* Enhanced task state with interval support */
static enhanced_watering_task_state_t enhanced_task_state;
static bool interval_task_system_initialized = false;

/* Helper: sync inline interval_config into the shadow interval_config_t shape */
static void interval_sync_to_shadow(watering_channel_t *channel)
{
    if (!channel) return;
    channel->interval_config_shadow.watering_minutes = channel->interval_config.watering_minutes;
    channel->interval_config_shadow.watering_seconds = channel->interval_config.watering_seconds;
    channel->interval_config_shadow.pause_minutes = channel->interval_config.pause_minutes;
    channel->interval_config_shadow.pause_seconds = channel->interval_config.pause_seconds;
    channel->interval_config_shadow.configured = channel->interval_config.configured;
    /* Reset runtime fields; controller will manage them */
    channel->interval_config_shadow.total_target = 0;
    channel->interval_config_shadow.cycles_completed = 0;
    channel->interval_config_shadow.currently_watering = false;
    channel->interval_config_shadow.phase_start_time = 0;
    channel->interval_config_shadow.phase_remaining_sec = 0;
}

int interval_task_init(void)
{
    memset(&enhanced_task_state, 0, sizeof(enhanced_watering_task_state_t));
    
    enhanced_task_state.current_active_task = NULL;
    enhanced_task_state.task_in_progress = false;
    enhanced_task_state.task_paused = false;
    enhanced_task_state.is_interval_mode = false;
    enhanced_task_state.current_phase = TASK_STATE_IDLE;
    enhanced_task_state.cycles_completed = 0;
    
    interval_task_system_initialized = true;
    
    LOG_INF("Interval task integration system initialized");
    return 0;
}

int interval_task_start(watering_task_t *task)
{
    if (!task || !task->channel) {
        LOG_ERR("Invalid task parameters");
        return -EINVAL;
    }

    if (!interval_task_system_initialized) {
        LOG_ERR("Interval task system not initialized");
        return -ENODEV;
    }

    uint8_t channel_id = task->channel - watering_channels;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %u", channel_id);
        return -EINVAL;
    }

    // Check if we should use interval mode
    bool should_use_interval;
    int ret = interval_task_should_use_interval(task, &should_use_interval);
    if (ret != 0) {
        LOG_ERR("Failed to determine interval mode usage");
        return ret;
    }

    // Initialize enhanced task state
    enhanced_task_state.current_active_task = task;
    enhanced_task_state.watering_start_time = k_uptime_get_32();
    enhanced_task_state.task_in_progress = true;
    enhanced_task_state.task_paused = false;
    enhanced_task_state.is_interval_mode = should_use_interval;
    enhanced_task_state.cycles_completed = 0;
    enhanced_task_state.phase_remaining_sec = 0;

    if (should_use_interval) {
        // Initialize interval controller
        watering_channel_t *channel = &watering_channels[channel_id];
        // Ensure the shadow config is in sync with the inline config before use
        interval_sync_to_shadow(channel);
        
        // Validate interval configuration
        ret = interval_task_validate_config(task);
        if (ret != 0) {
            LOG_ERR("Invalid interval configuration for channel %u", channel_id);
            return ret;
        }

        // Determine target based on watering mode
        uint32_t total_target;
        bool is_volume_based = false;
        
        switch (task->channel->watering_event.watering_mode) {
            case WATERING_BY_DURATION:
                total_target = task->channel->watering_event.watering.by_duration.duration_minutes * 60;
                is_volume_based = false;
                break;
            case WATERING_BY_VOLUME:
                total_target = task->by_volume.volume_liters * 1000; // Convert to ml
                is_volume_based = true;
                break;
            default:
                LOG_ERR("Interval mode not supported for watering mode %d", 
                        task->channel->watering_event.watering_mode);
                return -ENOTSUP;
        }

        // Initialize interval controller
    ret = interval_controller_init(&enhanced_task_state.interval_controller,
                       channel_id,
                       (interval_config_t *)&channel->interval_config_shadow,
                       total_target,
                       is_volume_based);
        if (ret != 0) {
            LOG_ERR("Failed to initialize interval controller");
            return ret;
        }

        // Start interval controller
        ret = interval_controller_start(&enhanced_task_state.interval_controller);
        if (ret != 0) {
            LOG_ERR("Failed to start interval controller");
            return ret;
        }

        enhanced_task_state.current_phase = TASK_STATE_WATERING;
        
        LOG_INF("Started interval mode task for channel %u: target=%u, volume_based=%s",
                channel_id, total_target, is_volume_based ? "true" : "false");
    } else {
        // Standard continuous watering mode
        enhanced_task_state.current_phase = TASK_STATE_WATERING;
        
        LOG_INF("Started continuous watering task for channel %u", channel_id);
    }

    // Update legacy task state for compatibility
    watering_task_state.current_active_task = task;
    watering_task_state.watering_start_time = enhanced_task_state.watering_start_time;
    watering_task_state.task_in_progress = true;
    watering_task_state.task_paused = false;
    watering_task_state.manual_override_active = false;

    return 0;
}

int interval_task_update(uint32_t current_volume, float flow_rate_ml_sec)
{
    if (!enhanced_task_state.task_in_progress) {
        return 0; // No active task
    }

    uint32_t current_time = k_uptime_get_32();

    if (enhanced_task_state.is_interval_mode) {
        // Update interval controller
        int ret = interval_controller_update(&enhanced_task_state.interval_controller,
                                            current_volume, flow_rate_ml_sec);
        if (ret != 0) {
            LOG_ERR("Failed to update interval controller");
            return ret;
        }

        // Get current status from interval controller
        enhanced_task_status_t status;
        ret = interval_controller_get_status(&enhanced_task_state.interval_controller, &status);
        if (ret != 0) {
            LOG_ERR("Failed to get interval controller status");
            return ret;
        }

        // Update enhanced task state
        enhanced_task_state.current_phase = status.state;
        enhanced_task_state.cycles_completed = status.interval.cycles_completed;

        // Get phase remaining time
        ret = interval_controller_get_phase_remaining(&enhanced_task_state.interval_controller,
                                                     &enhanced_task_state.phase_remaining_sec);
        if (ret != 0) {
            LOG_WRN("Failed to get phase remaining time");
            enhanced_task_state.phase_remaining_sec = 0;
        }

        // Calculate next phase time
        if (enhanced_task_state.phase_remaining_sec > 0) {
            enhanced_task_state.next_phase_time = current_time + 
                                                 (enhanced_task_state.phase_remaining_sec * 1000);
        } else {
            enhanced_task_state.next_phase_time = current_time;
        }

        LOG_DBG("Interval task update: phase=%d, remaining=%u sec, cycles=%u",
                enhanced_task_state.current_phase, enhanced_task_state.phase_remaining_sec,
                enhanced_task_state.cycles_completed);
    } else {
        // Standard continuous watering - just track elapsed time
        enhanced_task_state.current_phase = TASK_STATE_WATERING;
        enhanced_task_state.phase_remaining_sec = 0;
        enhanced_task_state.next_phase_time = 0;
    }

    // Update legacy task state
    watering_task_state.task_in_progress = enhanced_task_state.task_in_progress;
    watering_task_state.task_paused = enhanced_task_state.task_paused;
    watering_task_state.manual_override_active = false;

    return 0;
}

int interval_task_stop(const char *reason)
{
    if (!enhanced_task_state.task_in_progress) {
        return 0; // No active task
    }

    LOG_INF("Stopping task: %s", reason ? reason : "no reason");

    if (enhanced_task_state.is_interval_mode) {
        // Stop interval controller
        int ret = interval_controller_stop(&enhanced_task_state.interval_controller, reason);
        if (ret != 0) {
            LOG_ERR("Failed to stop interval controller");
            return ret;
        }
    }

    // Reset enhanced task state
    enhanced_task_state.current_active_task = NULL;
    enhanced_task_state.task_in_progress = false;
    enhanced_task_state.task_paused = false;
    enhanced_task_state.is_interval_mode = false;
    enhanced_task_state.current_phase = TASK_STATE_IDLE;
    enhanced_task_state.cycles_completed = 0;
    enhanced_task_state.phase_remaining_sec = 0;
    enhanced_task_state.next_phase_time = 0;

    // Update legacy task state
    watering_task_state.current_active_task = NULL;
    watering_task_state.task_in_progress = false;
    watering_task_state.task_paused = false;
    watering_task_state.manual_override_active = false;

    return 0;
}

int interval_task_is_complete(bool *is_complete)
{
    if (!is_complete) {
        LOG_ERR("Invalid is_complete pointer");
        return -EINVAL;
    }

    if (!enhanced_task_state.task_in_progress) {
        *is_complete = true;
        return 0;
    }

    if (enhanced_task_state.is_interval_mode) {
        // Check interval controller completion
        return interval_controller_is_complete(&enhanced_task_state.interval_controller, is_complete);
    } else {
        // For continuous mode, completion is handled by the standard task system
        *is_complete = false;
        return 0;
    }
}

int interval_task_get_status(enhanced_task_status_t *status)
{
    if (!status) {
        LOG_ERR("Invalid status pointer");
        return -EINVAL;
    }

    memset(status, 0, sizeof(enhanced_task_status_t));

    if (!enhanced_task_state.task_in_progress) {
        status->state = TASK_STATE_IDLE;
        status->mode = WATERING_BY_DURATION; // Default
        return 0;
    }

    if (enhanced_task_state.is_interval_mode) {
        // Get status from interval controller
        return interval_controller_get_status(&enhanced_task_state.interval_controller, status);
    } else {
        // Standard continuous watering status
        status->state = enhanced_task_state.current_phase;
        status->mode = (enhanced_watering_mode_t)enhanced_task_state.current_active_task->channel->watering_event.watering_mode;
        status->total_elapsed = k_uptime_get_32() - enhanced_task_state.watering_start_time;
        status->remaining_time = 0; // Calculated by caller for continuous mode
        status->total_volume = 0;   // Updated by caller
    }

    return 0;
}

int interval_task_should_use_interval(const watering_task_t *task, bool *should_use_interval)
{
    if (!task || !task->channel || !should_use_interval) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    uint8_t channel_id = task->channel - watering_channels;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    // Check if interval mode is configured and enabled
    // Work with the shadow config to match API type
    interval_sync_to_shadow(channel);
    bool is_configured;
    int ret = interval_timing_is_configured((const interval_config_t *)&channel->interval_config_shadow, &is_configured);
    if (ret != 0) {
        LOG_ERR("Failed to check interval configuration");
        return ret;
    }

    // Only use interval mode for duration and volume-based watering
    bool compatible_mode = (task->channel->watering_event.watering_mode == WATERING_BY_DURATION ||
                           task->channel->watering_event.watering_mode == WATERING_BY_VOLUME);

    *should_use_interval = is_configured && compatible_mode;

    LOG_DBG("Channel %u interval mode: configured=%s, compatible=%s, use=%s",
            channel_id, is_configured ? "yes" : "no", compatible_mode ? "yes" : "no",
            *should_use_interval ? "yes" : "no");

    return 0;
}

int interval_task_get_phase_info(bool *is_watering, uint32_t *phase_remaining_sec)
{
    if (!is_watering || !phase_remaining_sec) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (!enhanced_task_state.task_in_progress || !enhanced_task_state.is_interval_mode) {
        *is_watering = enhanced_task_state.current_phase == TASK_STATE_WATERING;
        *phase_remaining_sec = 0;
        return 0;
    }

    // Get phase info from interval controller
    int ret = interval_controller_is_watering(&enhanced_task_state.interval_controller, is_watering);
    if (ret != 0) {
        return ret;
    }

    *phase_remaining_sec = enhanced_task_state.phase_remaining_sec;
    return 0;
}

int interval_task_get_valve_control(bool *should_open_valve)
{
    if (!should_open_valve) {
        LOG_ERR("Invalid should_open_valve pointer");
        return -EINVAL;
    }

    if (!enhanced_task_state.task_in_progress) {
        *should_open_valve = false;
        return 0;
    }

    if (enhanced_task_state.is_interval_mode) {
        // For interval mode, valve should be open only during watering phase
        return interval_controller_is_watering(&enhanced_task_state.interval_controller, should_open_valve);
    } else {
        // For continuous mode, valve should always be open during task
        *should_open_valve = true;
        return 0;
    }
}

int interval_task_get_progress(uint8_t *progress_percent, uint32_t *cycles_remaining)
{
    if (!progress_percent || !cycles_remaining) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (!enhanced_task_state.task_in_progress) {
        *progress_percent = 0;
        *cycles_remaining = 0;
        return 0;
    }

    if (enhanced_task_state.is_interval_mode) {
        // Get progress from interval controller
        return interval_controller_get_progress(&enhanced_task_state.interval_controller,
                                               progress_percent, cycles_remaining);
    } else {
        // For continuous mode, progress calculation is handled elsewhere
        *progress_percent = 0;
        *cycles_remaining = 0;
        return 0;
    }
}

int interval_task_get_fallback_mode(const watering_task_t *task, 
                                   enhanced_watering_mode_t *fallback_mode)
{
    if (!task || !task->channel || !fallback_mode) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    // When interval mode is not configured, fall back to continuous watering
    *fallback_mode = (enhanced_watering_mode_t)task->channel->watering_event.watering_mode;

    LOG_DBG("Fallback mode for interval: %d", *fallback_mode);
    return 0;
}

int interval_task_validate_config(const watering_task_t *task)
{
    if (!task || !task->channel) {
        LOG_ERR("Invalid task parameters");
        return -EINVAL;
    }

    uint8_t channel_id = task->channel - watering_channels;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    // Validate interval timing configuration (use shadow type)
    interval_sync_to_shadow(channel);
    return interval_timing_validate_config((const interval_config_t *)&channel->interval_config_shadow);
}

int interval_task_handle_error(watering_error_t error, const char *error_message)
{
    LOG_ERR("Interval task error: %d - %s", error, error_message ? error_message : "no message");

    if (enhanced_task_state.is_interval_mode) {
        // Handle error in interval controller
        int ret = interval_controller_handle_error(&enhanced_task_state.interval_controller,
                                                  error, error_message);
        if (ret != 0) {
            LOG_ERR("Failed to handle interval controller error");
            return ret;
        }
    }

    // Update task state to error
    enhanced_task_state.current_phase = TASK_STATE_ERROR;
    
    return 0;
}

int interval_task_get_next_phase_time(uint32_t *next_phase_time_sec)
{
    if (!next_phase_time_sec) {
        LOG_ERR("Invalid next_phase_time_sec pointer");
        return -EINVAL;
    }

    if (!enhanced_task_state.task_in_progress || !enhanced_task_state.is_interval_mode) {
        *next_phase_time_sec = 0;
        return 0;
    }

    uint32_t current_time = k_uptime_get_32();
    if (enhanced_task_state.next_phase_time > current_time) {
        *next_phase_time_sec = (enhanced_task_state.next_phase_time - current_time) / 1000;
    } else {
        *next_phase_time_sec = 0;
    }

    return 0;
}

int interval_task_is_interval_mode(bool *is_interval_mode)
{
    if (!is_interval_mode) {
        LOG_ERR("Invalid is_interval_mode pointer");
        return -EINVAL;
    }

    *is_interval_mode = enhanced_task_state.is_interval_mode;
    return 0;
}

int interval_task_get_enhanced_state(enhanced_watering_task_state_t *task_state)
{
    if (!task_state) {
        LOG_ERR("Invalid task_state pointer");
        return -EINVAL;
    }

    *task_state = enhanced_task_state;
    return 0;
}

int interval_task_reset_state(void)
{
    memset(&enhanced_task_state, 0, sizeof(enhanced_watering_task_state_t));
    
    enhanced_task_state.current_phase = TASK_STATE_IDLE;
    enhanced_task_state.is_interval_mode = false;
    
    LOG_DBG("Reset interval task state");
    return 0;
}

int interval_task_is_supported(uint8_t channel_id, bool *is_supported)
{
    if (!is_supported || channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    // Interval mode is supported for all channels if properly configured
    watering_channel_t *channel = &watering_channels[channel_id];
    
    // Use shadow config for API compatibility
    interval_sync_to_shadow(channel);
    bool is_configured;
    int ret = interval_timing_is_configured((const interval_config_t *)&channel->interval_config_shadow, &is_configured);
    if (ret != 0) {
        *is_supported = false;
        return ret;
    }

    *is_supported = is_configured;
    return 0;
}

/**
 * @brief Initialize the interval task integration system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t interval_task_integration_init(void)
{
    static bool initialized = false;
    if (initialized) {
        return WATERING_SUCCESS;
    }

    int ret = interval_task_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize interval task system: %d", ret);
        return WATERING_ERROR_INTERVAL_CONFIG;
    }
    
    LOG_INF("Interval task integration system initialized");
    initialized = true;
    return WATERING_SUCCESS;
}
