#ifndef SOIL_MOISTURE_CONFIG_H
#define SOIL_MOISTURE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Default antecedent moisture used when nothing is configured */
#define SOIL_MOISTURE_DEFAULT_PCT 50u

int soil_moisture_config_init(void);

uint8_t soil_moisture_get_global_effective_pct(void);
uint8_t soil_moisture_get_effective_pct(uint8_t channel_id);

int soil_moisture_get_global(bool *enabled, uint8_t *moisture_pct);
int soil_moisture_set_global(bool enabled, uint8_t moisture_pct);

/* Like soil_moisture_get_global(), but also returns whether a value is stored in NVS. */
int soil_moisture_get_global_with_presence(bool *enabled, uint8_t *moisture_pct, bool *has_data);

int soil_moisture_get_channel_override(uint8_t channel_id, bool *enabled, uint8_t *moisture_pct);
int soil_moisture_set_channel_override(uint8_t channel_id, bool enabled, uint8_t moisture_pct);

/* Like soil_moisture_get_channel_override(), but also returns whether a value is stored in NVS. */
int soil_moisture_get_channel_override_with_presence(uint8_t channel_id, bool *enabled, uint8_t *moisture_pct, bool *has_data);

#endif /* SOIL_MOISTURE_CONFIG_H */
