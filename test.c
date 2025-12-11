#include <stdio.h>
struct __attribute__((packed)) history_env_daily_t {
    unsigned int date;
    short temp_min_x100;
    short temp_max_x100;
    short temp_avg_x100;
    unsigned short humid_min_x100;
    unsigned short humid_max_x100;
    unsigned short humid_avg_x100;
    unsigned short press_min_x10;
    unsigned short press_max_x10;
    unsigned short press_avg_x10;
    unsigned int total_rainfall_mm_x100;
    unsigned short watering_events;
    unsigned int total_volume_ml;
    unsigned short sample_count;
    unsigned char active_channels;
    unsigned char reserved[17];
};
int main() { printf(\"Size: %zu\n\", sizeof(struct history_env_daily_t)); return 0; }
