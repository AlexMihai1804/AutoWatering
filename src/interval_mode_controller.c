#include "interval_mode_controller.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>

LOG_MODULE_REGISTER(interval_controller, LOG_LEVEL_DBG);

int interval_controller_init(interval_controller_t *controller,
                            uint8_t channel_id,
                            interval_config_t *config,
                            uint32_t total_target,
                            bool is_volume_based)
{
    if (!controller || !config || channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid parameters for controller initialization");
        return -EINVAL;
    }

    // Validate interval configuration
    int ret = interval_timing_validate_config(config);
    if (ret != 0) {
        LOG_ERR("Invalid interval configuration for channel %u", channel_id);
        return ret;
    }

    if (total_target == 0) {
        LOG_ERR("Invalid total target: 0");
        return -EINVAL;
    }

    // Initialize controller structure
    memset(controller, 0, sizeof(interval_controller_t));
    
    controller->state = INTERVAL_STATE_IDLE;
    controller->config = config;
    controller->channel_id = channel_id;
    controller->total_target = total_target;
    controller->is_volume_based = is_volume_based;
    controller->flow_rate_ml_sec = 0.0f;
    controller->last_error = WATERING_SUCCESS;

    LOG_DBG("Initialized interval controller for channel %u: target=%u, volume_based=%s",
            channel_id, total_target, is_volume_based ? "true" : "false");

    return 0;
}

int interval_controller_start(interval_controller_t *controller)
{
    if (!controller) {
        LOG_ERR("Invalid controller pointer");
        return -EINVAL;
    }

    if (controller->state != INTERVAL_STATE_IDLE) {
        LOG_ERR("Controller not in idle state: %d", controller->state);
        return -EINVAL;
    }

    // Reset state for new task
    controller->task_start_time = k_uptime_get_32();
    controller->phase_start_time = controller->task_start_time;
    controller->total_elapsed = 0;
    controller->total_volume = 0;
    controller->cycles_completed = 0;
    controller->current_cycle_volume = 0;
    controller->last_update_time = controller->task_start_time;

    // Start with watering phase
    int ret = interval_controller_transition_state(controller, INTERVAL_STATE_WATERING);
    if (ret != 0) {
        LOG_ERR("Failed to transition to watering state");
        return ret;
    }

    // Update interval config state
    ret = interval_timing_update_phase(controller->config, true, controller->phase_start_time);
    if (ret != 0) {
        LOG_ERR("Failed to update interval timing phase");
        return ret;
    }

    LOG_INF("Started interval mode for channel %u", controller->channel_id);
    return 0;
}

int interval_controller_update(interval_controller_t *controller,
                              uint32_t current_volume,
                              float flow_rate_ml_sec)
{
    if (!controller) {
        LOG_ERR("Invalid controller pointer");
        return -EINVAL;
    }

    if (controller->state == INTERVAL_STATE_IDLE || controller->state == INTERVAL_STATE_COMPLETED) {
        return 0; // Nothing to update
    }

    uint32_t current_time = k_uptime_get_32();
    controller->last_update_time = current_time;
    controller->total_elapsed = current_time - controller->task_start_time;
    controller->total_volume = current_volume;
    controller->flow_rate_ml_sec = flow_rate_ml_sec;

    // Check if we should switch phases
    bool should_switch;
    interval_state_t next_state;
    int ret = interval_controller_should_switch_phase(controller, &should_switch, &next_state);
    if (ret != 0) {
        return ret;
    }

    if (should_switch) {
        ret = interval_controller_transition_state(controller, next_state);
        if (ret != 0) {
            LOG_ERR("Failed to transition to state %d", next_state);
            return ret;
        }
    }

    // Check if task is complete
    bool is_complete;
    ret = interval_controller_is_complete(controller, &is_complete);
    if (ret != 0) {
        return ret;
    }

    if (is_complete && controller->state != INTERVAL_STATE_COMPLETED) {
        ret = interval_controller_transition_state(controller, INTERVAL_STATE_COMPLETED);
        if (ret != 0) {
            LOG_ERR("Failed to transition to completed state");
            return ret;
        }
    }

    // Update interval timing phase information
    bool currently_watering = (controller->state == INTERVAL_STATE_WATERING);
    ret = interval_timing_update_phase(controller->config, currently_watering, 
                                      controller->phase_start_time);
    if (ret != 0) {
        LOG_WRN("Failed to update interval timing phase");
    }

    return 0;
}

int interval_controller_stop(interval_controller_t *controller,
                            const char *reason)
{
    if (!controller) {
        LOG_ERR("Invalid controller pointer");
        return -EINVAL;
    }

    if (controller->state == INTERVAL_STATE_IDLE) {
        return 0; // Already stopped
    }

    LOG_INF("Stopping interval controller for channel %u: %s", 
            controller->channel_id, reason ? reason : "no reason");

    // Transition to completed state
    int ret = interval_controller_transition_state(controller, INTERVAL_STATE_COMPLETED);
    if (ret != 0) {
        LOG_ERR("Failed to transition to completed state");
        return ret;
    }

    return 0;
}

int interval_controller_is_complete(const interval_controller_t *controller,
                                   bool *is_complete)
{
    if (!controller || !is_complete) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    *is_complete = false;

    if (controller->state == INTERVAL_STATE_COMPLETED || 
        controller->state == INTERVAL_STATE_ERROR) {
        *is_complete = true;
        return 0;
    }

    // Check completion based on mode
    if (controller->is_volume_based) {
        // Volume-based completion
        *is_complete = (controller->total_volume >= controller->total_target);
    } else {
        // Duration-based completion
        *is_complete = (controller->total_elapsed >= (controller->total_target * 1000));
    }

    return 0;
}

int interval_controller_get_status(const interval_controller_t *controller,
                                  enhanced_task_status_t *status)
{
    if (!controller || !status) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    memset(status, 0, sizeof(enhanced_task_status_t));

    // Map interval state to enhanced task state
    switch (controller->state) {
        case INTERVAL_STATE_IDLE:
            status->state = TASK_STATE_IDLE;
            break;
        case INTERVAL_STATE_WATERING:
            status->state = TASK_STATE_WATERING;
            break;
        case INTERVAL_STATE_PAUSING:
            status->state = TASK_STATE_PAUSING;
            break;
        case INTERVAL_STATE_COMPLETED:
            status->state = TASK_STATE_COMPLETED;
            break;
        case INTERVAL_STATE_ERROR:
            status->state = TASK_STATE_ERROR;
            break;
    }

    status->mode = WATERING_BY_INTERVAL;
    status->interval = *controller->config;
    status->total_elapsed = controller->total_elapsed;
    status->total_volume = controller->total_volume;

    // Calculate remaining time
    if (controller->is_volume_based) {
        // For volume-based, estimate remaining time based on flow rate
        if (controller->flow_rate_ml_sec > 0.0f) {
            uint32_t remaining_volume = (controller->total_volume < controller->total_target) ?
                                       (controller->total_target - controller->total_volume) : 0;
            status->remaining_time = (uint32_t)(remaining_volume / controller->flow_rate_ml_sec);
        } else {
            status->remaining_time = 0;
        }
    } else {
        // For duration-based, calculate remaining time directly
        uint32_t target_ms = controller->total_target * 1000;
        status->remaining_time = (controller->total_elapsed < target_ms) ?
                                (target_ms - controller->total_elapsed) / 1000 : 0;
    }

    return 0;
}

int interval_controller_get_phase_remaining(const interval_controller_t *controller,
                                           uint32_t *remaining_sec)
{
    if (!controller || !remaining_sec) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (controller->state != INTERVAL_STATE_WATERING && 
        controller->state != INTERVAL_STATE_PAUSING) {
        *remaining_sec = 0;
        return 0;
    }

    bool currently_watering = (controller->state == INTERVAL_STATE_WATERING);
    return interval_timing_get_phase_remaining(controller->config, 
                                              controller->phase_start_time,
                                              currently_watering, 
                                              remaining_sec);
}

int interval_controller_is_watering(const interval_controller_t *controller,
                                   bool *is_watering)
{
    if (!controller || !is_watering) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    *is_watering = (controller->state == INTERVAL_STATE_WATERING);
    return 0;
}

int interval_controller_is_pausing(const interval_controller_t *controller,
                                  bool *is_pausing)
{
    if (!controller || !is_pausing) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    *is_pausing = (controller->state == INTERVAL_STATE_PAUSING);
    return 0;
}

int interval_controller_get_progress(const interval_controller_t *controller,
                                    uint8_t *progress_percent,
                                    uint32_t *cycles_remaining)
{
    if (!controller || !progress_percent || !cycles_remaining) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    // Calculate progress based on mode
    uint32_t progress;
    if (controller->is_volume_based) {
        progress = (controller->total_volume * 100) / controller->total_target;
    } else {
        uint32_t target_ms = controller->total_target * 1000;
        progress = (controller->total_elapsed * 100) / target_ms;
    }

    *progress_percent = (progress > 100) ? 100 : (uint8_t)progress;

    // Calculate remaining cycles
    uint32_t cycle_duration_sec;
    int ret = interval_timing_get_cycle_duration(controller->config, &cycle_duration_sec);
    if (ret != 0) {
        *cycles_remaining = 0;
        return ret;
    }

    if (controller->is_volume_based) {
        // For volume-based, estimate cycles based on flow rate
        if (controller->flow_rate_ml_sec > 0.0f) {
            uint32_t watering_sec = interval_get_watering_duration_sec(controller->config);
            uint32_t volume_per_cycle = (uint32_t)(watering_sec * controller->flow_rate_ml_sec);
            if (volume_per_cycle > 0) {
                uint32_t remaining_volume = (controller->total_volume < controller->total_target) ?
                                           (controller->total_target - controller->total_volume) : 0;
                *cycles_remaining = (remaining_volume + volume_per_cycle - 1) / volume_per_cycle;
            } else {
                *cycles_remaining = 0;
            }
        } else {
            *cycles_remaining = 0;
        }
    } else {
        // For duration-based, calculate remaining cycles directly
        uint32_t remaining_sec = (controller->total_elapsed < (controller->total_target * 1000)) ?
                                ((controller->total_target * 1000) - controller->total_elapsed) / 1000 : 0;
        *cycles_remaining = (remaining_sec + cycle_duration_sec - 1) / cycle_duration_sec;
    }

    return 0;
}

int interval_controller_transition_state(interval_controller_t *controller,
                                        interval_state_t new_state)
{
    if (!controller) {
        LOG_ERR("Invalid controller pointer");
        return -EINVAL;
    }

    interval_state_t old_state = controller->state;
    
    // Validate state transition
    bool valid_transition = false;
    switch (old_state) {
        case INTERVAL_STATE_IDLE:
            valid_transition = (new_state == INTERVAL_STATE_WATERING);
            break;
        case INTERVAL_STATE_WATERING:
            valid_transition = (new_state == INTERVAL_STATE_PAUSING || 
                               new_state == INTERVAL_STATE_COMPLETED ||
                               new_state == INTERVAL_STATE_ERROR);
            break;
        case INTERVAL_STATE_PAUSING:
            valid_transition = (new_state == INTERVAL_STATE_WATERING ||
                               new_state == INTERVAL_STATE_COMPLETED ||
                               new_state == INTERVAL_STATE_ERROR);
            break;
        case INTERVAL_STATE_COMPLETED:
        case INTERVAL_STATE_ERROR:
            valid_transition = (new_state == INTERVAL_STATE_IDLE);
            break;
    }

    if (!valid_transition) {
        LOG_ERR("Invalid state transition: %d -> %d", old_state, new_state);
        return -EINVAL;
    }

    // Perform state-specific actions
    uint32_t current_time = k_uptime_get_32();
    
    switch (new_state) {
        case INTERVAL_STATE_WATERING:
            controller->phase_start_time = current_time;
            controller->current_cycle_volume = 0;
            if (old_state == INTERVAL_STATE_PAUSING) {
                // Completed a cycle
                controller->cycles_completed++;
            }
            break;
            
        case INTERVAL_STATE_PAUSING:
            controller->phase_start_time = current_time;
            break;
            
        case INTERVAL_STATE_COMPLETED:
            // Task completed successfully
            break;
            
        case INTERVAL_STATE_ERROR:
            // Error occurred
            break;
            
        case INTERVAL_STATE_IDLE:
            // Reset controller
            interval_controller_reset(controller);
            break;
    }

    controller->state = new_state;

    LOG_DBG("State transition for channel %u: %s -> %s",
            controller->channel_id,
            interval_controller_state_to_string(old_state),
            interval_controller_state_to_string(new_state));

    return 0;
}

int interval_controller_should_switch_phase(const interval_controller_t *controller,
                                           bool *should_switch,
                                           interval_state_t *next_state)
{
    if (!controller || !should_switch || !next_state) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    *should_switch = false;
    *next_state = controller->state;

    if (controller->state != INTERVAL_STATE_WATERING && 
        controller->state != INTERVAL_STATE_PAUSING) {
        return 0; // No phase switching needed
    }

    // Check if current phase duration has elapsed
    uint32_t phase_remaining_sec;
    int ret = interval_controller_get_phase_remaining(controller, &phase_remaining_sec);
    if (ret != 0) {
        return ret;
    }

    if (phase_remaining_sec == 0) {
        *should_switch = true;
        
        if (controller->state == INTERVAL_STATE_WATERING) {
            *next_state = INTERVAL_STATE_PAUSING;
        } else {
            *next_state = INTERVAL_STATE_WATERING;
        }
    }

    return 0;
}

const char* interval_controller_state_to_string(interval_state_t state)
{
    switch (state) {
        case INTERVAL_STATE_IDLE:
            return "IDLE";
        case INTERVAL_STATE_WATERING:
            return "WATERING";
        case INTERVAL_STATE_PAUSING:
            return "PAUSING";
        case INTERVAL_STATE_COMPLETED:
            return "COMPLETED";
        case INTERVAL_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

int interval_controller_handle_error(interval_controller_t *controller,
                                    watering_error_t error,
                                    const char *error_message)
{
    if (!controller) {
        LOG_ERR("Invalid controller pointer");
        return -EINVAL;
    }

    controller->last_error = error;
    
    LOG_ERR("Interval controller error for channel %u: %d - %s",
            controller->channel_id, error, error_message ? error_message : "no message");

    // Transition to error state
    return interval_controller_transition_state(controller, INTERVAL_STATE_ERROR);
}

int interval_controller_reset(interval_controller_t *controller)
{
    if (!controller) {
        LOG_ERR("Invalid controller pointer");
        return -EINVAL;
    }

    controller->state = INTERVAL_STATE_IDLE;
    controller->task_start_time = 0;
    controller->phase_start_time = 0;
    controller->total_elapsed = 0;
    controller->total_volume = 0;
    controller->cycles_completed = 0;
    controller->current_cycle_volume = 0;
    controller->flow_rate_ml_sec = 0.0f;
    controller->last_update_time = 0;
    controller->last_error = WATERING_SUCCESS;

    // Reset interval timing state
    if (controller->config) {
        interval_timing_reset_state(controller->config);
    }

    LOG_DBG("Reset interval controller for channel %u", controller->channel_id);
    return 0;
}

int interval_controller_validate(const interval_controller_t *controller)
{
    if (!controller) {
        LOG_ERR("Invalid controller pointer");
        return -EINVAL;
    }

    if (controller->channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %u", controller->channel_id);
        return -EINVAL;
    }

    if (!controller->config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    return interval_timing_validate_config(controller->config);
}