#include "speedometer.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "speedometer";
static const float STATIONARY_THRESHOLD_KMH = 2.0f;
static const float MOVING_THRESHOLD_KMH = 3.0f;
static const int64_t STATIONARY_TIME_US = 3000000LL;
static const int64_t MOVING_TIME_US = 1000000LL;
static const int64_t GPS_STALE_TIME_US = 1500000LL;
static const float SPEED_FILTER_ALPHA = 0.25f;

void speedometer_init(speedometer_t* s, float initial_total_distance_m)
{
  memset(s, 0, sizeof(*s));
  s->total_distance_m = fmaxf(0.0f, initial_total_distance_m);
}

void speedometer_update(speedometer_t* s, const gps_data_t* gps, int64_t now_us)
{
  if (!s || !gps)
    return;

  s->gps_fix = gps->fix_valid;
  s->satellites = gps->satellites;
  s->raw_speed_kmh = gps->fix_valid ? fmaxf(0.0f, gps->speed_kmh) : 0.0f;
  s->diagnostic_sample_count++;

  int64_t sample_us = gps->sample_time_us ? gps->sample_time_us : now_us;
  if (!gps->fix_valid)
  {
    s->filtered_speed_kmh = 0.0f;
    s->display_speed_kmh = 0.0f;
    s->stationary = true;
    s->stationary_below_since_us = 0;
    s->moving_above_since_us = 0;
    s->last_sample_us = sample_us;
    return;
  }

  if (s->last_sample_us == 0 && s->filtered_speed_kmh == 0.0f)
  {
    s->filtered_speed_kmh = s->raw_speed_kmh;
  }
  else
  {
    s->filtered_speed_kmh += SPEED_FILTER_ALPHA * (s->raw_speed_kmh - s->filtered_speed_kmh);
  }

  float dt = s->last_sample_us > 0 ? (float)(sample_us - s->last_sample_us) / 1000000.0f : 0.0f;
  s->last_sample_us = sample_us;

  if (s->stationary)
  {
    if (s->raw_speed_kmh >= MOVING_THRESHOLD_KMH)
    {
      if (s->moving_above_since_us == 0)
        s->moving_above_since_us = sample_us;
      if (sample_us - s->moving_above_since_us >= MOVING_TIME_US)
      {
        s->stationary = false;
        s->moving_above_since_us = 0;
        s->last_sample_us = sample_us;
      }
    }
    else
    {
      s->moving_above_since_us = 0;
    }
  }
  else if (s->raw_speed_kmh <= STATIONARY_THRESHOLD_KMH)
  {
    if (s->stationary_below_since_us == 0)
      s->stationary_below_since_us = sample_us;
    if (sample_us - s->stationary_below_since_us >= STATIONARY_TIME_US)
    {
      s->stationary = true;
      s->stationary_below_since_us = 0;
      s->moving_above_since_us = 0;
    }
  }
  else
  {
    s->stationary_below_since_us = 0;
  }

  s->display_speed_kmh = s->stationary ? 0.0f : s->filtered_speed_kmh;
  if (s->display_speed_kmh > 999.0f)
    s->display_speed_kmh = 999.0f;

  if (dt <= 0.0f || dt > 2.0f || s->stationary)
    return;

  if (!s->trip_started && s->raw_speed_kmh >= MOVING_THRESHOLD_KMH)
  {
    s->trip_started = true;
    s->trip_start_us = sample_us;
  }

  // Integrate GNSS speed rather than noisy point-to-point position changes.
  // Trapezoidal integration is intentionally avoided here because raw speed
  // from the previous sample is not retained separately; at 5-10 Hz the
  // midpoint error is negligible for this display application.
  if (s->raw_speed_kmh >= MOVING_THRESHOLD_KMH)
  {
    float meters = (s->filtered_speed_kmh / 3.6f) * dt;
    if (meters >= 0.0f && meters < 100.0f)
    {
      s->trip_distance_m += meters;
      s->total_distance_m += meters;
    }
  }
}

void speedometer_tick(speedometer_t* s, int64_t now_us)
{
  if (!s || s->last_sample_us == 0 || now_us - s->last_sample_us <= GPS_STALE_TIME_US)
    return;

  s->gps_fix = false;
  s->raw_speed_kmh = 0.0f;
  s->filtered_speed_kmh = 0.0f;
  s->display_speed_kmh = 0.0f;
  s->stationary = true;
  s->stationary_below_since_us = 0;
  s->moving_above_since_us = 0;
}

void speedometer_log_diagnostics(speedometer_t* s, int64_t now_us)
{
  if (!s || (s->last_diagnostic_us != 0 && now_us - s->last_diagnostic_us < 1000000LL))
    return;

  s->last_diagnostic_us = now_us;
  ESP_LOGI(TAG, "GPS: raw=%.2fm/s filtered=%.2fm/s display=%.1fkm/h state=%s samples=%u fix=%s",
           s->raw_speed_kmh / 3.6f, s->filtered_speed_kmh / 3.6f, s->display_speed_kmh,
           s->stationary ? "STATIONARY" : "MOVING", s->diagnostic_sample_count,
           s->gps_fix ? "VALID" : "INVALID");
  s->diagnostic_sample_count = 0;
}

void speedometer_reset_trip(speedometer_t* s)
{
  if (!s)
    return;
  s->trip_distance_m = 0.0f;
  s->trip_started = false;
  s->trip_start_us = 0;
}

float speedometer_average_kmh(const speedometer_t* s)
{
  if (!s || !s->trip_started || s->trip_start_us == 0 || s->last_sample_us <= s->trip_start_us)
  {
    return 0.0f;
  }

  float hours = (float)(s->last_sample_us - s->trip_start_us) / 3600000000.0f;
  if (hours <= 0.0f)
    return 0.0f;
  float km = s->trip_distance_m / 1000.0f;
  float average = km / hours;
  if (average < 0.0f)
    average = 0.0f;
  if (average > 999.0f)
    average = 999.0f;
  return average;
}
