#include "speedometer.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "speedometer";
static const float STATIONARY_THRESHOLD_KMH = 2.0f;
static const float MOVING_THRESHOLD_KMH = 3.0f;
static const float STATIONARY_MEDIAN_THRESHOLD_KMH = 1.5f;
static const float STATIONARY_RATIO_THRESHOLD = 0.80f;
static const float MOVING_RATIO_THRESHOLD = 0.60f;
static const int64_t GPS_STALE_TIME_US = 1500000LL;
static const float LOW_SPEED_ALPHA = 0.30f;
static const float MEDIUM_SPEED_ALPHA = 0.15f;
static const float HIGH_SPEED_ALPHA = 0.08f;
static const float STABLE_ALPHA_SCALE = 0.55f;
static const float INSTANT_ALPHA = 0.40f;

static void push_sample(float* values, uint8_t* count, uint8_t capacity, float value)
{
  if (!values || !count || capacity == 0)
    return;

  if (*count < capacity)
  {
    values[(*count)++] = value;
    return;
  }

  for (uint8_t i = 1; i < capacity; ++i)
  {
    values[i - 1] = values[i];
  }
  values[capacity - 1] = value;
}

static float median_of_values(const float* values, int count)
{
  if (!values || count <= 0)
    return 0.0f;

  float copy[32];
  for (int i = 0; i < count && i < 32; ++i)
    copy[i] = values[i];

  for (int i = 0; i < count - 1; ++i)
  {
    for (int j = i + 1; j < count; ++j)
    {
      if (copy[j] < copy[i])
      {
        float tmp = copy[i];
        copy[i] = copy[j];
        copy[j] = tmp;
      }
    }
  }

  if (count % 2 == 0)
    return 0.5f * (copy[count / 2 - 1] + copy[count / 2]);
  return copy[count / 2];
}

static float mean_of_values(const float* values, int count)
{
  if (!values || count <= 0)
    return 0.0f;

  float sum = 0.0f;
  for (int i = 0; i < count; ++i)
    sum += values[i];
  return sum / (float)count;
}

static float stddev_of_values(const float* values, int count)
{
  if (!values || count <= 1)
    return 0.0f;

  float mean = mean_of_values(values, count);
  float variance = 0.0f;
  for (int i = 0; i < count; ++i)
  {
    float delta = values[i] - mean;
    variance += delta * delta;
  }
  variance /= (float)(count - 1);
  return sqrtf(variance);
}

static float robust_speed_estimate(const speedometer_t* s)
{
  if (!s || s->sample_count == 0)
    return 0.0f;

  float median = median_of_values(s->sample_window, s->sample_count);
  if (s->sample_count < 3)
    return median;

  float rejected_limit = 8.0f + fminf(0.18f * median, 18.0f);
  float sample = s->raw_speed_kmh;
  if (fabsf(sample - median) > rejected_limit)
    return median;
  return sample;
}

static bool is_stationary_window(const speedometer_t* s)
{
  if (!s || s->sample_count == 0)
    return false;

  float median = median_of_values(s->sample_window, s->sample_count);
  int below_two = 0;
  for (uint8_t i = 0; i < s->sample_count; ++i)
  {
    if (s->sample_window[i] < STATIONARY_THRESHOLD_KMH)
      below_two++;
  }

  float below_ratio = (float)below_two / (float)s->sample_count;
  return (median < STATIONARY_MEDIAN_THRESHOLD_KMH) || (below_ratio >= STATIONARY_RATIO_THRESHOLD);
}

static bool is_moving_window(const speedometer_t* s)
{
  if (!s || s->sample_count == 0)
    return false;

  float median = median_of_values(s->sample_window, s->sample_count);
  int above_three = 0;
  for (uint8_t i = 0; i < s->sample_count; ++i)
  {
    if (s->sample_window[i] >= MOVING_THRESHOLD_KMH)
      above_three++;
  }

  float above_ratio = (float)above_three / (float)s->sample_count;
  return (median >= 2.5f) || (above_ratio >= MOVING_RATIO_THRESHOLD);
}

static void reset_speed_tracking(speedometer_t* s)
{
  if (!s)
    return;

  memset(s->sample_window, 0, sizeof(s->sample_window));
  memset(s->robust_window, 0, sizeof(s->robust_window));
  s->sample_count = 0;
  s->robust_count = 0;
  s->raw_speed_kmh = 0.0f;
  s->robust_speed_kmh = 0.0f;
  s->instant_speed_kmh = 0.0f;
  s->reference_speed_kmh = 0.0f;
  s->filtered_speed_kmh = 0.0f;
  s->display_speed_kmh = 0.0f;
  s->stationary = true;
  s->speed_stable = false;
  s->stationary_below_since_us = 0;
  s->moving_above_since_us = 0;
}

void speedometer_init(speedometer_t* s, float initial_total_distance_m)
{
  memset(s, 0, sizeof(*s));
  s->total_distance_m = fmaxf(0.0f, initial_total_distance_m);
  s->stationary = true;
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
    s->speed_stable = false;
    s->stationary_below_since_us = 0;
    s->moving_above_since_us = 0;
    s->last_sample_us = sample_us;
    return;
  }

  if (s->last_sample_us == 0)
  {
    s->instant_speed_kmh = s->raw_speed_kmh;
    s->reference_speed_kmh = s->raw_speed_kmh;
    s->filtered_speed_kmh = s->raw_speed_kmh;
  }

  push_sample(s->sample_window, &s->sample_count, 32, s->raw_speed_kmh);
  s->robust_speed_kmh = robust_speed_estimate(s);
  push_sample(s->robust_window, &s->robust_count, 24, s->robust_speed_kmh);

  if (s->robust_count >= 6)
  {
    float stable_window[24];
    for (uint8_t i = 0; i < s->robust_count; ++i)
      stable_window[i] = s->robust_window[i];

    float mean = mean_of_values(stable_window, s->robust_count);
    float stddev = stddev_of_values(stable_window, s->robust_count);
    float span = 0.0f;
    float min_v = stable_window[0];
    float max_v = stable_window[0];
    for (uint8_t i = 0; i < s->robust_count; ++i)
    {
      if (stable_window[i] < min_v)
        min_v = stable_window[i];
      if (stable_window[i] > max_v)
        max_v = stable_window[i];
    }
    span = max_v - min_v;
    float trend = fabsf(stable_window[s->robust_count - 1] - stable_window[0]);
    float stability_limit = 1.0f + 0.03f * mean;
    s->speed_stable = (stddev <= stability_limit) && (span <= stability_limit) &&
                      (trend <= stability_limit * 0.75f);
  }
  else
  {
    s->speed_stable = false;
  }

  float alpha_reference =
      s->robust_speed_kmh < 15.0f
          ? LOW_SPEED_ALPHA
          : (s->robust_speed_kmh < 50.0f ? MEDIUM_SPEED_ALPHA : HIGH_SPEED_ALPHA);
  if (s->speed_stable)
    alpha_reference *= STABLE_ALPHA_SCALE;
  if (fabsf(s->robust_speed_kmh - s->reference_speed_kmh) > 8.0f && s->reference_speed_kmh > 0.0f)
    alpha_reference = fmaxf(alpha_reference, MEDIUM_SPEED_ALPHA);

  float alpha_instant = INSTANT_ALPHA;
  if (s->robust_speed_kmh < 15.0f)
    alpha_instant = 0.55f;
  else if (s->robust_speed_kmh < 50.0f)
    alpha_instant = 0.45f;
  else if (s->robust_speed_kmh < 100.0f)
    alpha_instant = 0.28f;
  else
    alpha_instant = 0.18f;

  if (s->stationary)
  {
    if (is_moving_window(s))
    {
      s->stationary = false;
      s->instant_speed_kmh = s->robust_speed_kmh;
      s->reference_speed_kmh = s->robust_speed_kmh;
      s->filtered_speed_kmh = s->robust_speed_kmh;
      s->moving_above_since_us = sample_us;
      s->stationary_below_since_us = 0;
    }
    else
    {
      s->instant_speed_kmh = 0.0f;
      s->reference_speed_kmh = 0.0f;
      s->filtered_speed_kmh = 0.0f;
      s->display_speed_kmh = 0.0f;
      s->stationary_below_since_us = 0;
      s->moving_above_since_us = 0;
      s->last_sample_us = sample_us;
      return;
    }
  }
  else
  {
    if (is_stationary_window(s))
    {
      s->stationary = true;
      s->instant_speed_kmh = 0.0f;
      s->reference_speed_kmh = 0.0f;
      s->filtered_speed_kmh = 0.0f;
      s->display_speed_kmh = 0.0f;
      s->stationary_below_since_us = sample_us;
      s->moving_above_since_us = 0;
      s->last_sample_us = sample_us;
      return;
    }
  }

  s->instant_speed_kmh += alpha_instant * (s->robust_speed_kmh - s->instant_speed_kmh);
  s->reference_speed_kmh += alpha_reference * (s->robust_speed_kmh - s->reference_speed_kmh);
  s->filtered_speed_kmh = s->reference_speed_kmh;

  s->display_speed_kmh = s->stationary ? 0.0f : s->reference_speed_kmh;
  if (s->display_speed_kmh > 999.0f)
    s->display_speed_kmh = 999.0f;

  float dt = s->last_sample_us > 0 ? (float)(sample_us - s->last_sample_us) / 1000000.0f : 0.0f;
  s->last_sample_us = sample_us;

  if (dt <= 0.0f || dt > 2.0f || s->stationary)
    return;

  if (!s->trip_started && s->raw_speed_kmh >= MOVING_THRESHOLD_KMH)
  {
    s->trip_started = true;
    s->trip_start_us = sample_us;
  }

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
  reset_speed_tracking(s);
  s->stationary = true;
  s->last_sample_us = 0;
}

void speedometer_log_diagnostics(speedometer_t* s, int64_t now_us)
{
  if (!s || (s->last_diagnostic_us != 0 && now_us - s->last_diagnostic_us < 1000000LL))
    return;

  s->last_diagnostic_us = now_us;
  ESP_LOGD(
      TAG,
      "GPS raw=%.1f robust=%.1f instant=%.1f reference=%.1f display=%.1f %s %s samples=%u sats=%u",
      s->raw_speed_kmh, s->robust_speed_kmh, s->instant_speed_kmh, s->reference_speed_kmh,
      s->display_speed_kmh, s->stationary ? "STATIONARY" : "MOVING",
      s->speed_stable ? "STABLE" : "UNSTABLE", s->diagnostic_sample_count, s->satellites);
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
