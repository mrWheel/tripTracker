#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "gps.h"

typedef struct
{
  float raw_speed_kmh;
  float robust_speed_kmh;
  float instant_speed_kmh;
  float reference_speed_kmh;
  float filtered_speed_kmh;
  float display_speed_kmh;
  float trip_distance_m;
  float total_distance_m;
  bool gps_fix;
  bool stationary;
  bool speed_stable;
  uint8_t satellites;

  float sample_window[32];
  float robust_window[24];
  uint8_t sample_count;
  uint8_t robust_count;

  bool trip_started;
  int64_t trip_start_us;
  int64_t last_sample_us;
  int64_t stationary_below_since_us;
  int64_t moving_above_since_us;
  int64_t last_diagnostic_us;
  uint32_t diagnostic_sample_count;
} speedometer_t;

void speedometer_init(speedometer_t* s, float initial_total_distance_m);
void speedometer_update(speedometer_t* s, const gps_data_t* gps, int64_t now_us);
void speedometer_tick(speedometer_t* s, int64_t now_us);
void speedometer_log_diagnostics(speedometer_t* s, int64_t now_us);
void speedometer_reset_trip(speedometer_t* s);
float speedometer_average_kmh(const speedometer_t* s);
