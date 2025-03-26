#ifndef WATERING_INTERNAL_H
#define WATERING_INTERNAL_H

#include "watering.h"

/* Shared global variables */
extern watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];
extern watering_status_t system_status;

/* For flow monitoring access - forward declaration */
struct watering_task_state_t {
    watering_task_t *current_active_task;
    uint32_t watering_start_time;
};

/* External reference to the task state */
extern struct watering_task_state_t watering_task_state;

/* Constants for volume-to-pulse conversion */
#define DEFAULT_PULSES_PER_LITER 750 // Higher default value for more sensitive sensors

/* Constants for anomaly checking */
#define FLOW_CHECK_THRESHOLD_MS 5000 // Check every 5 seconds
#define MAX_FLOW_ERROR_ATTEMPTS 3 // Number of attempts before entering fault mode
#define UNEXPECTED_FLOW_THRESHOLD 10 // Number of pulses indicating unexpected flow

/* Task management */
void tasks_init(void);
void watering_start_task(watering_task_t *task);
bool watering_stop_current_task(void);

/* Flow monitoring */
void flow_monitor_init(void);
void check_flow_anomalies(void);

/* Configuration handling */
void config_init(void);

#endif // WATERING_INTERNAL_H
