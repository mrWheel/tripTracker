#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gps.h"

//-- Mount point used by the SD card FatFs volume, shared with the webserver file manager.
#define SDCARD_MOUNT_POINT "/sdcard"

//-- Upper bound on the number of trip files sdcard_list_trip_files() reports;
//-- keep main/app_main.c's trip-list array sized to this same value.
#define SDCARD_MAX_LISTED_TRIPS 48

typedef struct
{
  bool mounted;
  uint64_t total_bytes;
  uint64_t free_bytes;
  uint8_t free_percent;
} sdcard_status_t;

typedef struct
{
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  float distance_m;
  //-- Filename without extension, e.g. "trip-20260915-143022"; used to locate
  //-- the matching CSV file for sdcard_get_trip_details().
  char base_name[32];
} sdcard_trip_summary_t;

typedef struct
{
  float distance_m;
  uint32_t duration_s;
  float avg_speed_kmh;
  //-- Highest altitude minus lowest altitude recorded during the trip.
  float altitude_diff_m;
  //-- Time of the last recorded CSV row; the start time is the trip's
  //-- filename date/time (sdcard_trip_summary_t).
  uint8_t end_hour;
  uint8_t end_minute;
  bool valid;
} sdcard_trip_details_t;

esp_err_t sdcard_init(void);
esp_err_t sdcard_reset_trip(void);
esp_err_t sdcard_format(void);
esp_err_t sdcard_remove_small_trip_files(void);
//-- Deletes trip file pairs whose .gpx file is smaller than min_gpx_bytes;
//-- skips the currently active (recording) trip.
esp_err_t sdcard_remove_undersized_trip_files(size_t min_gpx_bytes);
esp_err_t sdcard_append_fix(const gps_data_t* gps, float trip_distance_m, bool stationary);
uint32_t sdcard_get_entry_count(void);
void sdcard_get_status(sdcard_status_t* status);
uint16_t sdcard_get_trip_number(void);

//-- Copies the "EEYYMMDD-HHmmSS" portion of the active trip filename into out.
void sdcard_get_active_trip_datetime(char* out, size_t out_size);
size_t sdcard_list_trip_files(sdcard_trip_summary_t* out, size_t max_count);
//-- Reads the CSV file matching base_name (without extension) to compute
//-- duration, average speed and altitude range for the [Trip Info] screen.
esp_err_t sdcard_get_trip_details(const char* base_name, sdcard_trip_details_t* out);
esp_err_t sdcard_finish(void);
