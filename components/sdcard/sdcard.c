#include "sdcard.h"

#include <math.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT SDCARD_MOUNT_POINT
#define SD_CS GPIO_NUM_4
#define SD_HOST SPI3_HOST
#define SDCARD_NVS_NAMESPACE "sdcard"
#define SDCARD_NVS_ACTIVE_GPX "active_gpx"

//-- TEMPORARY debug switch: keep trip files with fewer than 20 entries
//-- instead of deleting them, so recorded points can be inspected while the
//-- GPX/CSV export bug is being diagnosed. Set back to 0 once confirmed fixed.
#define SDCARD_KEEP_SMALL_TRIP_FILES 1

//-- A fix is skipped when stationary or when it barely moved since the last recorded point.
static const float MIN_MOVEMENT_METERS = 1.5f;

static const char* TAG = "sdcard";
static sdmmc_card_t* s_card;
static int s_trip_gpx_fd = -1;
static int s_trip_csv_fd = -1;
static bool s_mounted;
static uint32_t s_last_sequence;
static char s_trip_gpx_path[64];
static char s_trip_csv_path[64];
static bool s_has_last_coord;
static double s_last_lat;
static double s_last_lon;
static float s_trip_distance_m;
static float s_last_written_speed_kmh;
static uint32_t s_entry_count;
static uint16_t s_current_trip_number;
static bool s_waiting_for_gps_time;
static bool s_waiting_for_new_filename;
static char s_closed_gpx_path[64];
//-- Set while [WiFi Menu] has closed every open trip file; no new trip file
//-- is created and no GPS fixes are recorded until it is cleared again.
static bool s_recording_suspended;
//-- Optional sink for short, user-facing status lines (see sdcard_set_status_log()).
static sdcard_status_log_fn_t s_status_log_fn;

//-- Log a short status line via ESP_LOGI and forward it to s_status_log_fn,
//-- e.g. for display on the [Start Webserver] screen.
static void report_status(const char* fmt, ...)
{
  char message[64];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  ESP_LOGI(TAG, "%s", message);
  if (s_status_log_fn)
  {
    s_status_log_fn(message, SDCARD_LOG_INFO);
  }
}

//-- Same as report_status(), but flags the line for on-screen success/green
//-- highlighting (e.g. a filename that was just closed or removed).
static void report_success(const char* fmt, ...)
{
  char message[64];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  ESP_LOGI(TAG, "%s", message);
  if (s_status_log_fn)
  {
    s_status_log_fn(message, SDCARD_LOG_SUCCESS);
  }
}

void sdcard_set_status_log(sdcard_status_log_fn_t fn)
{
  s_status_log_fn = fn;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool is_european_position(double latitude_deg, double longitude_deg)
{
  return latitude_deg >= -90.0 && latitude_deg <= 90.0 && longitude_deg >= -180.0 &&
         longitude_deg <= 180.0 && latitude_deg >= 35.0 && latitude_deg <= 72.0 &&
         longitude_deg >= -25.0 && longitude_deg <= 45.0;
}

static bool is_europe_dst_active(uint16_t year, uint8_t month, uint8_t day)
{
  (void)year;
  if (month < 3 || month > 10)
    return false;
  if (month > 3 && month < 10)
    return true;
  if (month == 3)
    return day >= 25;
  if (month == 10)
    return day <= 25;
  return false;
}

static int determine_timezone_offset_hours(double latitude_deg, double longitude_deg, uint16_t year,
                                           uint8_t month, uint8_t day)
{
  if (is_european_position(latitude_deg, longitude_deg))
  {
    int base_offset_hours = 1;
    if (longitude_deg < -10.0)
      base_offset_hours = 0;
    else if (longitude_deg >= 20.0)
      base_offset_hours = 2;
    if (is_europe_dst_active(year, month, day))
      base_offset_hours += 1;
    return base_offset_hours;
  }

  int fallback_offset_hours = (int)lround(longitude_deg / 15.0);
  if (fallback_offset_hours > 14)
    fallback_offset_hours = 14;
  else if (fallback_offset_hours < -12)
    fallback_offset_hours = -12;
  return fallback_offset_hours;
}

static void gps_utc_to_local(const gps_data_t* gps, uint16_t* out_year, uint8_t* out_month,
                             uint8_t* out_day, uint8_t* out_hour, uint8_t* out_minute,
                             uint8_t* out_second, int* out_offset_hours)
{
  if (!gps || !out_year || !out_month || !out_day || !out_hour || !out_minute || !out_second ||
      !out_offset_hours)
  {
    return;
  }

  *out_offset_hours = determine_timezone_offset_hours(gps->latitude_deg, gps->longitude_deg,
                                                      gps->year, gps->month, gps->day);

  setenv("TZ", "UTC", 1);
  tzset();

  struct tm utc_tm = {0};
  utc_tm.tm_year = gps->year - 1900;
  utc_tm.tm_mon = gps->month - 1;
  utc_tm.tm_mday = gps->day;
  utc_tm.tm_hour = gps->hour;
  utc_tm.tm_min = gps->minute;
  utc_tm.tm_sec = gps->second;
  utc_tm.tm_isdst = 0;

  time_t utc_epoch = mktime(&utc_tm);
  if (utc_epoch == (time_t)-1)
  {
    *out_year = gps->year;
    *out_month = gps->month;
    *out_day = gps->day;
    *out_hour = gps->hour;
    *out_minute = gps->minute;
    *out_second = gps->second;
    return;
  }

  time_t local_epoch = utc_epoch + ((time_t)*out_offset_hours * 3600LL);
  struct tm local_tm = {0};
  gmtime_r(&local_epoch, &local_tm);

  *out_year = (uint16_t)(local_tm.tm_year + 1900);
  *out_month = (uint8_t)(local_tm.tm_mon + 1);
  *out_day = (uint8_t)local_tm.tm_mday;
  *out_hour = (uint8_t)local_tm.tm_hour;
  *out_minute = (uint8_t)local_tm.tm_min;
  *out_second = (uint8_t)local_tm.tm_sec;
}

//-- Create both date-time-based trip filenames in the form trip-EEYYMMDD-HHmmSS.ext.
static void build_trip_filenames(const gps_data_t* gps)
{
  uint16_t local_year = gps->year;
  uint8_t local_month = gps->month;
  uint8_t local_day = gps->day;
  uint8_t local_hour = gps->hour;
  uint8_t local_minute = gps->minute;
  uint8_t local_second = gps->second;
  int offset_hours = 0;
  gps_utc_to_local(gps, &local_year, &local_month, &local_day, &local_hour, &local_minute,
                   &local_second, &offset_hours);

  //-- The "O" (open) marker right before ".gpx" marks a trip file that is
  //-- still being recorded; it is dropped only once the trip is properly closed.
  snprintf(s_trip_gpx_path, sizeof(s_trip_gpx_path),
           SD_MOUNT_POINT "/trip-%04d%02d%02d-%02d%02d%02dO.gpx", (int)local_year, (int)local_month,
           (int)local_day, (int)local_hour, (int)local_minute, (int)local_second);
  snprintf(s_trip_csv_path, sizeof(s_trip_csv_path),
           SD_MOUNT_POINT "/trip-%04d%02d%02d-%02d%02d%02d.csv", (int)local_year, (int)local_month,
           (int)local_day, (int)local_hour, (int)local_minute, (int)local_second);
}

//-- Calculate distance in meters between two lat/lon coordinates using the Haversine formula.
static double calculate_distance_m(double lat1, double lon1, double lat2, double lon2)
{
  double lat1_rad = lat1 * (M_PI / 180.0);
  double lat2_rad = lat2 * (M_PI / 180.0);
  double delta_lat = (lat2 - lat1) * (M_PI / 180.0);
  double delta_lon = (lon2 - lon1) * (M_PI / 180.0);

  double sin_dlat = sin(delta_lat / 2.0);
  double sin_dlon = sin(delta_lon / 2.0);

  double a = sin_dlat * sin_dlat + cos(lat1_rad) * cos(lat2_rad) * sin_dlon * sin_dlon;
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

  const double earth_radius_m = 6371000.0;
  return earth_radius_m * c;
}

static void clear_status(sdcard_status_t* status)
{
  memset(status, 0, sizeof(*status));
}

static esp_err_t update_status(sdcard_status_t* status)
{
  FATFS* filesystem = NULL;
  DWORD free_clusters = 0;
  if (!s_mounted || !s_card)
  {
    clear_status(status);
    return ESP_FAIL;
  }

  status->mounted = true;
  FRESULT result = f_getfree("0:", &free_clusters, &filesystem);
  if (result != FR_OK || !filesystem)
  {
    ESP_LOGE(TAG, "Unable to read SD filesystem statistics: %d", result);
    status->mounted = false;
    status->total_bytes = 0;
    status->free_bytes = 0;
    status->free_percent = 0;
    return ESP_FAIL;
  }

  uint64_t sector_size = s_card->csd.sector_size;
  uint64_t total_sectors = ((uint64_t)(filesystem->n_fatent - 2)) * filesystem->csize;
  uint64_t free_sectors = (uint64_t)free_clusters * filesystem->csize;
  status->total_bytes = total_sectors * sector_size;
  status->free_bytes = free_sectors * sector_size;
  status->free_percent =
      status->total_bytes > 0 ? (uint8_t)((status->free_bytes * 100U) / status->total_bytes) : 0;
  if (status->free_percent > 100)
    status->free_percent = 100;
  return ESP_OK;
}

static esp_err_t write_text(int file, const char* path, const char* text)
{
  size_t length = strlen(text);
  const char* cursor = text;
  while (length > 0)
  {
    ssize_t written = write(file, cursor, length);
    if (written <= 0)
    {
      ESP_LOGE(TAG, "Write failed for %s, errno=%d", path, errno);
      return ESP_FAIL;
    }
    cursor += written;
    length -= (size_t)written;
  }
  return ESP_OK;
}

//-- Add the GPX closing tags to an open descriptor only if they are not
//-- already present. Per-point appends no longer keep them up to date, so a
//-- trip file only becomes valid, closed GPX again once it is actually finalized.
static esp_err_t ensure_gpx_file_closed(int fd, const char* path)
{
  off_t file_size = lseek(fd, 0, SEEK_END);
  size_t read_size = file_size > 128 ? 128 : (size_t)file_size;
  char tail[129] = {0};
  bool has_closing_tags = false;
  if (file_size >= 0 && lseek(fd, -((off_t)read_size), SEEK_END) >= 0 &&
      read(fd, tail, read_size) == (ssize_t)read_size)
  {
    has_closing_tags = strstr(tail, "</gpx>") != NULL;
  }
  if (lseek(fd, 0, SEEK_END) < 0)
  {
    return ESP_FAIL;
  }
  if (has_closing_tags)
  {
    return ESP_OK;
  }
  if (write_text(fd, path, "  </trkseg></trk>\n</gpx>\n") != ESP_OK || fsync(fd) != 0)
  {
    return ESP_FAIL;
  }
  return ESP_OK;
}

//-- Drop the "O" (open) marker from a closed GPX filename, e.g.
//-- "trip-20260917-143022O.gpx" => "trip-20260917-143022.gpx".
static esp_err_t rename_gpx_remove_open_marker(const char* open_path, char* closed_path,
                                               size_t closed_path_size)
{
  size_t length = strlen(open_path);
  if (length < 5 || strcmp(open_path + length - 5, "O.gpx") != 0)
  {
    snprintf(closed_path, closed_path_size, "%s", open_path);
    return ESP_OK;
  }
  snprintf(closed_path, closed_path_size, "%.*s.gpx", (int)(length - 5), open_path);
  ESP_LOGI(TAG, "rename(\"%s\", \"%s\")", open_path, closed_path);
  if (rename(open_path, closed_path) != 0)
  {
    ESP_LOGE(TAG, "Cannot rename %s to %s, errno=%d", open_path, closed_path, errno);
    snprintf(closed_path, closed_path_size, "%s", open_path);
    return ESP_FAIL;
  }
  {
    //-- Reported as two short lines (label, then the closed filename in
    //-- green) so neither one can run past the [Start Webserver] screen
    //-- width; the filename is the renamed one, without the "O" marker.
    const char* base = strrchr(closed_path, '/');
    base = base ? base + 1 : closed_path;
    report_status("Closing tripFile:");
    report_success("%s", base);
  }
  return ESP_OK;
}

//-- Finalize and rename the active GPX file. This is the only place closing
//-- tags are (re)written now that per-point appends no longer maintain them.
static esp_err_t finalize_active_gpx_and_rename(void)
{
  if (s_trip_gpx_fd < 0)
    return ESP_OK;

  esp_err_t result = ESP_OK;
  if (ensure_gpx_file_closed(s_trip_gpx_fd, s_trip_gpx_path) != ESP_OK)
  {
    result = ESP_FAIL;
  }
  if (close(s_trip_gpx_fd) != 0)
  {
    result = ESP_FAIL;
  }
  s_trip_gpx_fd = -1;

  char closed_path[sizeof(s_trip_gpx_path)];
  if (rename_gpx_remove_open_marker(s_trip_gpx_path, closed_path, sizeof(closed_path)) == ESP_OK)
  {
    snprintf(s_trip_gpx_path, sizeof(s_trip_gpx_path), "%s", closed_path);
  }
  else
  {
    result = ESP_FAIL;
  }
  return result;
}

static bool is_trip_filename(const char* name)
{
  size_t length = strlen(name);
  return strncmp(name, "trip-", 5) == 0 && length > 9 &&
         (strcmp(name + length - 4, ".gpx") == 0 || strcmp(name + length - 4, ".csv") == 0);
}

//-- Parse the date/time out of a trip-EEYYMMDD-HHmm[SS].gpx filename. The time
//-- part is accepted as either 4 digits (legacy files without seconds) or 6
//-- digits (the current trip-EEYYMMDD-HHmmSS format), so older trip files
//-- still list correctly.
static bool parse_trip_filename(const char* name, sdcard_trip_summary_t* out)
{
  const char* date_start = name + 5;
  const char* dash = strchr(date_start, '-');
  const char* dot = strrchr(name, '.');
  if (!dash || !dot || dot <= dash + 1)
  {
    return false;
  }

  size_t date_len = (size_t)(dash - date_start);
  size_t time_len = (size_t)(dot - (dash + 1));
  if (date_len != 8 || (time_len != 4 && time_len != 6))
  {
    return false;
  }

  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (sscanf(date_start, "%4d%2d%2d", &year, &month, &day) != 3)
  {
    return false;
  }
  if (time_len == 6)
  {
    if (sscanf(dash + 1, "%2d%2d%2d", &hour, &minute, &second) != 3)
    {
      return false;
    }
  }
  else if (sscanf(dash + 1, "%2d%2d", &hour, &minute) != 2)
  {
    return false;
  }

  out->year = (uint16_t)year;
  out->month = (uint8_t)month;
  out->day = (uint8_t)day;
  out->hour = (uint8_t)hour;
  out->minute = (uint8_t)minute;
  out->second = (uint8_t)second;
  return true;
}

//-- Read the cumulative distance_m value from the last <trkpt> in a closed
//-- GPX file by scanning the tail of the file, same technique as
//-- recover_active_trip().
static float read_trip_gpx_last_distance(const char* path)
{
  int file = open(path, O_RDONLY);
  if (file < 0)
  {
    return 0.0f;
  }

  off_t file_size = lseek(file, 0, SEEK_END);
  size_t read_size = file_size > 512 ? 512 : (size_t)file_size;
  char tail[513] = {0};
  if (file_size < 0 || lseek(file, -((off_t)read_size), SEEK_END) < 0 ||
      read(file, tail, read_size) != (ssize_t)read_size)
  {
    close(file);
    return 0.0f;
  }
  close(file);

  float distance = 0.0f;
  const char* search = tail;
  const char* found;
  while ((found = strstr(search, "<distance_m>")) != NULL)
  {
    sscanf(found + strlen("<distance_m>"), "%f", &distance);
    search = found + strlen("<distance_m>");
  }
  return distance;
}

static int compare_trip_summary_desc(const void* a, const void* b)
{
  const sdcard_trip_summary_t* ta = (const sdcard_trip_summary_t*)a;
  const sdcard_trip_summary_t* tb = (const sdcard_trip_summary_t*)b;
  if (ta->year != tb->year)
    return (int)tb->year - (int)ta->year;
  if (ta->month != tb->month)
    return (int)tb->month - (int)ta->month;
  if (ta->day != tb->day)
    return (int)tb->day - (int)ta->day;
  if (ta->hour != tb->hour)
    return (int)tb->hour - (int)ta->hour;
  if (ta->minute != tb->minute)
    return (int)tb->minute - (int)ta->minute;
  return (int)tb->second - (int)ta->second;
}

static uint32_t count_trip_entries(const char* path, bool gpx)
{
  FILE* file = fopen(path, "r");
  if (!file)
    return 0;

  char line[256];
  uint32_t count = 0;
  while (fgets(line, sizeof(line), file))
  {
    if ((gpx && strstr(line, "<trkpt ")) || (!gpx && strstr(line, "\n") && line[0] != 't'))
    {
      ++count;
    }
  }
  fclose(file);
  return count;
}

static void restore_last_trip_point(void)
{
  FILE* file = fopen(s_trip_csv_path, "r");
  if (!file)
    return;

  char line[256];
  char date[16];
  char time[16];
  double latitude;
  double longitude;
  float altitude;
  float speed;
  float course;
  unsigned satellites;
  float distance;
  while (fgets(line, sizeof(line), file))
  {
    if (sscanf(line, "%15[^,],%15[^,],%lf,%lf,%f,%f,%f,%u,%f", date, time, &latitude, &longitude,
               &altitude, &speed, &course, &satellites, &distance) == 9)
    {
      s_last_lat = latitude;
      s_last_lon = longitude;
      s_trip_distance_m = distance;
      s_last_written_speed_kmh = speed;
      s_has_last_coord = true;
    }
  }
  fclose(file);
}

static esp_err_t close_trip_files(void)
{
  esp_err_t result = ESP_OK;
  if (s_trip_gpx_fd >= 0)
  {
    if (finalize_active_gpx_and_rename() != ESP_OK)
    {
      result = ESP_FAIL;
    }
  }
  if (s_trip_csv_fd >= 0)
  {
    if (fsync(s_trip_csv_fd) != 0 || close(s_trip_csv_fd) != 0)
    {
      result = ESP_FAIL;
    }
    s_trip_csv_fd = -1;
  }
  return result;
}

static esp_err_t save_active_gpx_path(void)
{
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(SDCARD_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (result != ESP_OK)
    return result;
  result = nvs_set_str(handle, SDCARD_NVS_ACTIVE_GPX, s_trip_gpx_path);
  if (result == ESP_OK)
    result = nvs_commit(handle);
  nvs_close(handle);
  if (result == ESP_OK)
  {
    ESP_LOGI(TAG, "NVS active_gpx => [%s]", s_trip_gpx_path);
  }
  else
  {
    ESP_LOGE(TAG, "Cannot save NVS active_gpx [%s]: %s", s_trip_gpx_path, esp_err_to_name(result));
  }
  return result;
}

static void clear_active_gpx_path(void)
{
  nvs_handle_t handle;
  if (nvs_open(SDCARD_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_erase_key(handle, SDCARD_NVS_ACTIVE_GPX);
  nvs_commit(handle);
  nvs_close(handle);
  ESP_LOGI(TAG, "NVS active_gpx => [cleared]");
}

static esp_err_t recover_active_trip(void)
{
  nvs_handle_t handle = 0;
  size_t path_size = sizeof(s_trip_gpx_path);
  if (nvs_open(SDCARD_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK ||
      nvs_get_str(handle, SDCARD_NVS_ACTIVE_GPX, s_trip_gpx_path, &path_size) != ESP_OK)
  {
    if (handle)
      nvs_close(handle);
    ESP_LOGI(TAG, "NVS active_gpx => [not found]");
    return ESP_ERR_NOT_FOUND;
  }
  nvs_close(handle);
  ESP_LOGI(TAG, "NVS active_gpx <= [%s]", s_trip_gpx_path);

  size_t path_length = strlen(s_trip_gpx_path);
  if (path_length <= 4 || strcmp(s_trip_gpx_path + path_length - 4, ".gpx") != 0)
  {
    clear_active_gpx_path();
    return ESP_ERR_INVALID_ARG;
  }

  snprintf(s_trip_csv_path, sizeof(s_trip_csv_path), "%.*s.csv", (int)(path_length - 4),
           s_trip_gpx_path);

  int file = open(s_trip_gpx_path, O_RDWR);
  if (file < 0)
  {
    ESP_LOGW(TAG, "Active GPX file is unavailable: %s", s_trip_gpx_path);
    clear_active_gpx_path();
    return ESP_ERR_NOT_FOUND;
  }

  off_t file_size = lseek(file, 0, SEEK_END);
  size_t read_size = file_size > 512 ? 512 : (size_t)file_size;
  char tail[513] = {0};
  if (file_size < 0 || lseek(file, -((off_t)read_size), SEEK_END) < 0 ||
      read(file, tail, read_size) != (ssize_t)read_size)
  {
    ESP_LOGE(TAG, "Cannot read the end of %s, errno=%d", s_trip_gpx_path, errno);
    close(file);
    return ESP_FAIL;
  }

  char* closing_tags = strstr(tail, "</trkseg>");
  if (closing_tags)
  {
    off_t truncate_at = file_size - (off_t)read_size + (off_t)(closing_tags - tail);
    if (ftruncate(file, truncate_at) != 0)
    {
      ESP_LOGE(TAG, "Cannot reopen active GPX %s, errno=%d", s_trip_gpx_path, errno);
      close(file);
      return ESP_FAIL;
    }
  }
  close(file);

  //-- O_RDWR so remove_gpx_closing_tags() can keep reading the tail of the
  //-- file on this same descriptor before every append, as in create_trip_file().
  s_trip_gpx_fd = open(s_trip_gpx_path, O_RDWR | O_APPEND);
  s_trip_csv_fd = open(s_trip_csv_path, O_WRONLY | O_APPEND);
  if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
  {
    close_trip_files();
    ESP_LOGE(TAG, "Cannot reopen active trip files, errno=%d", errno);
    return ESP_FAIL;
  }
  s_entry_count = count_trip_entries(s_trip_gpx_path, true);
  restore_last_trip_point();
  s_waiting_for_gps_time = false;
  ESP_LOGI(TAG, "Resumed active trip %s with %u entries", s_trip_gpx_path, s_entry_count);
  return ESP_OK;
}

esp_err_t sdcard_remove_small_trip_files(void)
{
  if (!s_mounted)
  {
    return ESP_ERR_INVALID_STATE;
  }

  bool waiting_for_new_trip = false;
  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    if (close_trip_files() != ESP_OK)
    {
      return ESP_FAIL;
    }
    bool too_small = s_entry_count < 20;
#if SDCARD_KEEP_SMALL_TRIP_FILES
    too_small = false;
#endif
    if (too_small)
    {
      remove(s_trip_gpx_path);
      remove(s_trip_csv_path);
      waiting_for_new_trip = true;
    }
    else
    {
      s_trip_gpx_fd = open(s_trip_gpx_path, O_RDWR | O_APPEND);
      s_trip_csv_fd = open(s_trip_csv_path, O_WRONLY | O_APPEND);
      if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
      {
        close_trip_files();
        return ESP_FAIL;
      }
    }
  }

  DIR* directory = opendir(SD_MOUNT_POINT);
  if (!directory)
  {
    ESP_LOGE(TAG, "Cannot open SD card directory for trip cleanup");
    return ESP_FAIL;
  }

  struct dirent* entry;
  esp_err_t result = ESP_OK;
  while ((entry = readdir(directory)) != NULL)
  {
    if (!is_trip_filename(entry->d_name))
    {
      continue;
    }

    char path[sizeof(s_trip_gpx_path)];
    int written = snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
      result = ESP_FAIL;
      continue;
    }

    bool small_file;
#if SDCARD_KEEP_SMALL_TRIP_FILES
    //-- The result is discarded below anyway while this debug switch is on;
    //-- skip the full-file entry count so cleanup doesn't scan every trip
    //-- file's content on every [Start Webserver] entry.
    small_file = false;
#else
    bool gpx = strcmp(path + strlen(path) - 4, ".gpx") == 0;
    small_file = count_trip_entries(path, gpx) < 20;
#endif
    if (small_file && remove(path) != 0)
    {
      ESP_LOGW(TAG, "Cannot delete small trip file %s", path);
      result = ESP_FAIL;
    }
    else if (small_file)
    {
      report_status("Removing empty tripFile:");
      report_success("%s", entry->d_name);
    }
  }
  closedir(directory);

  s_waiting_for_gps_time = waiting_for_new_trip;
  if (waiting_for_new_trip)
  {
    s_last_sequence = 0;
    s_has_last_coord = false;
    s_entry_count = 0;
  }
  return result;
}

esp_err_t sdcard_remove_undersized_trip_files(size_t min_gpx_bytes)
{
  if (!s_mounted)
  {
    return ESP_ERR_INVALID_STATE;
  }

  DIR* directory = opendir(SD_MOUNT_POINT);
  if (!directory)
  {
    ESP_LOGE(TAG, "Cannot open SD card directory for undersized trip cleanup");
    return ESP_FAIL;
  }

  struct dirent* entry;
  esp_err_t result = ESP_OK;
  while ((entry = readdir(directory)) != NULL)
  {
    size_t name_length = strlen(entry->d_name);
    if (!is_trip_filename(entry->d_name) || name_length < 4 ||
        strcmp(entry->d_name + name_length - 4, ".gpx") != 0)
    {
      continue;
    }

    char gpx_path[sizeof(s_trip_gpx_path)];
    int written = snprintf(gpx_path, sizeof(gpx_path), SD_MOUNT_POINT "/%s", entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(gpx_path))
    {
      result = ESP_FAIL;
      continue;
    }

    //-- Never delete the trip that is currently being recorded.
    if (s_trip_gpx_fd >= 0 && strcmp(gpx_path, s_trip_gpx_path) == 0)
    {
      continue;
    }

    //-- stat() is unreliable for size on this FatFs VFS (see the f_getfree()
    //-- note above for statvfs()); use the same open+lseek technique already
    //-- proven by read_trip_gpx_last_distance() instead.
    int size_fd = open(gpx_path, O_RDONLY);
    if (size_fd < 0)
    {
      continue;
    }
    off_t file_size = lseek(size_fd, 0, SEEK_END);
    close(size_fd);
    if (file_size < 0 || (size_t)file_size >= min_gpx_bytes)
    {
      continue;
    }
#if SDCARD_KEEP_SMALL_TRIP_FILES
    continue;
#endif

    char csv_path[sizeof(s_trip_csv_path)];
    snprintf(csv_path, sizeof(csv_path), "%.*s.csv", written - 4, gpx_path);

    if (remove(gpx_path) != 0)
    {
      ESP_LOGW(TAG, "Cannot delete undersized trip file %s", gpx_path);
      result = ESP_FAIL;
    }
    else
    {
      report_status("Removing empty tripFile:");
      report_success("%s", entry->d_name);
    }
    //-- The matching CSV may already be missing; a failed remove() here is not an error.
    remove(csv_path);
  }
  closedir(directory);
  return result;
}

//-- Close the currently active trip file (if any) and finalize/rename every
//-- other GPX file still carrying the "O" open marker, e.g. left over from a
//-- crash or unexpected power loss. No new trip file is created here; that
//-- only happens once [WiFi Menu] is left again (see sdcard_reset_trip()).
esp_err_t sdcard_close_all_open_trip_files(void)
{
  if (!s_mounted)
  {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t result = close_trip_files();
  clear_active_gpx_path();
  s_waiting_for_gps_time = false;
  s_waiting_for_new_filename = false;
  s_recording_suspended = true;

  DIR* directory = opendir(SD_MOUNT_POINT);
  if (!directory)
  {
    ESP_LOGE(TAG, "Cannot open SD card directory to close open trip files");
    return ESP_FAIL;
  }

  struct dirent* entry;
  while ((entry = readdir(directory)) != NULL)
  {
    size_t name_length = strlen(entry->d_name);
    if (name_length < 5 || strcmp(entry->d_name + name_length - 5, "O.gpx") != 0)
    {
      continue;
    }

    char open_path[sizeof(s_trip_gpx_path)];
    int written = snprintf(open_path, sizeof(open_path), SD_MOUNT_POINT "/%s", entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(open_path))
    {
      result = ESP_FAIL;
      continue;
    }

    int fd = open(open_path, O_RDWR | O_APPEND);
    if (fd < 0)
    {
      ESP_LOGW(TAG, "Cannot open orphaned trip file %s, errno=%d", open_path, errno);
      result = ESP_FAIL;
      continue;
    }

    bool ok = ensure_gpx_file_closed(fd, open_path) == ESP_OK;
    if (close(fd) != 0)
    {
      ok = false;
    }
    char closed_path[sizeof(s_trip_gpx_path)];
    if (ok && rename_gpx_remove_open_marker(open_path, closed_path, sizeof(closed_path)) != ESP_OK)
    {
      ok = false;
    }
    if (!ok)
    {
      ESP_LOGW(TAG, "Cannot finalize orphaned trip file %s", open_path);
      result = ESP_FAIL;
    }
  }
  closedir(directory);
  return result;
}

static esp_err_t create_trip_file(const gps_data_t* gps)
{
  if (!gps || !gps->date_valid)
  {
    s_waiting_for_gps_time = true;
    return ESP_ERR_INVALID_STATE;
  }
  build_trip_filenames(gps);
  if (s_waiting_for_new_filename && strcmp(s_trip_gpx_path, s_closed_gpx_path) == 0)
  {
    s_waiting_for_gps_time = true;
    return ESP_ERR_INVALID_STATE;
  }
  s_waiting_for_new_filename = false;

  //-- The GPX descriptor must stay readable: remove_gpx_closing_tags() reads the
  //-- current tail of the file before every write to locate and strip the
  //-- closing tags, which fails with EBADF on an O_WRONLY descriptor.
  s_trip_gpx_fd = open(s_trip_gpx_path, O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0666);
  s_trip_csv_fd = open(s_trip_csv_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
  if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
  {
    ESP_LOGE(TAG, "Cannot create GPX/CSV trip files");
    close_trip_files();
    remove(s_trip_gpx_path);
    remove(s_trip_csv_path);
    return ESP_FAIL;
  }
  if (write_text(s_trip_gpx_fd, s_trip_gpx_path,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<gpx version=\"1.1\" creator=\"tripTracker\" "
                 "xmlns=\"http://www.topografix.com/GPX/1/1\" "
                 "xmlns:trkptx=\"https://triptracker.local/gpx\">\n"
                 "  <trk><name>GPS Trip</name><trkseg>\n") != ESP_OK ||
      write_text(s_trip_csv_fd, s_trip_csv_path,
                 "date,time,latitude_deg,longitude_deg,altitude_m,speed_kmh,course_deg,satellites,"
                 "distance_m\n") != ESP_OK ||
      fsync(s_trip_gpx_fd) != 0 || fsync(s_trip_csv_fd) != 0)
  {
    close_trip_files();
    remove(s_trip_gpx_path);
    remove(s_trip_csv_path);
    return ESP_FAIL;
  }

  s_current_trip_number = 0;
  s_last_sequence = 0;
  s_has_last_coord = false;
  s_trip_distance_m = 0.0f;
  s_last_written_speed_kmh = 0.0f;
  s_entry_count = 0;
  s_waiting_for_gps_time = false;
  if (save_active_gpx_path() != ESP_OK)
  {
    ESP_LOGE(TAG, "Cannot persist active trip path");
    close_trip_files();
    remove(s_trip_gpx_path);
    remove(s_trip_csv_path);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Writing %s and %s", s_trip_gpx_path, s_trip_csv_path);
  return ESP_OK;
}

esp_err_t sdcard_init(void)
{
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_HOST;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = SD_CS;
  slot_config.host_id = SD_HOST;

  esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 4,
      .allocation_unit_size = 16 * 1024,
  };

  esp_err_t err =
      esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(err));
    return err;
  }

  s_mounted = true;
  esp_err_t recover_err = recover_active_trip();
  if (recover_err == ESP_OK)
  {
    return ESP_OK;
  }
  err = create_trip_file(NULL);
  if (err == ESP_ERR_INVALID_STATE)
  {
    ESP_LOGI(TAG, "Waiting for GPS date/time before creating the trip file");
    return ESP_OK;
  }
  if (err != ESP_OK)
  {
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    return err;
  }
  return ESP_OK;
}

esp_err_t sdcard_reset_trip(void)
{
  if (!s_mounted)
    return ESP_ERR_INVALID_STATE;
  s_recording_suspended = false;
  bool retained_closed_trip = false;
  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    snprintf(s_closed_gpx_path, sizeof(s_closed_gpx_path), "%s", s_trip_gpx_path);
    if (close_trip_files() != ESP_OK)
    {
      return ESP_FAIL;
    }
    bool too_small = s_entry_count < 20;
#if SDCARD_KEEP_SMALL_TRIP_FILES
    too_small = false;
#endif
    if (too_small)
    {
      remove(s_trip_gpx_path);
      remove(s_trip_csv_path);
    }
    else
    {
      retained_closed_trip = true;
    }
    clear_active_gpx_path();
  }
  s_waiting_for_new_filename = retained_closed_trip;
  esp_err_t err = create_trip_file(NULL);
  if (err == ESP_ERR_INVALID_STATE)
  {
    ESP_LOGI(TAG, "Trip reset is waiting for GPS date/time");
    return ESP_OK;
  }
  return err;
}

esp_err_t sdcard_format(void)
{
  if (!s_mounted || !s_card)
  {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    if (close_trip_files() != ESP_OK)
    {
      return ESP_FAIL;
    }
  }

  esp_err_t err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card format failed: %s", esp_err_to_name(err));
    return err;
  }

  s_last_sequence = 0;
  esp_err_t create_err = create_trip_file(NULL);
  if (create_err == ESP_ERR_INVALID_STATE)
  {
    ESP_LOGI(TAG, "SD format complete; waiting for GPS date/time before creating the trip file");
    return ESP_OK;
  }
  return create_err;
}

esp_err_t sdcard_append_fix(const gps_data_t* gps, float trip_distance_m, bool stationary)
{
  if (!s_mounted || !gps)
  {
    return ESP_ERR_INVALID_STATE;
  }

  //-- No active trip while [WiFi Menu] has closed every open trip file; a new
  //-- one only starts after the menu is left (see sdcard_reset_trip()).
  if (s_recording_suspended)
  {
    return ESP_OK;
  }

  if (s_waiting_for_gps_time)
  {
    if (!gps->date_valid)
    {
      return ESP_OK;
    }
    if (create_trip_file(gps) != ESP_OK)
    {
      return ESP_FAIL;
    }
  }

  if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
  {
    return ESP_ERR_INVALID_STATE;
  }
  if (!gps->fix_valid || gps->sequence == s_last_sequence)
  {
    return ESP_OK;
  }
  if (gps->sequence == 0)
    return ESP_ERR_INVALID_ARG;

  //-- Write every fix unless the speedometer marks it stationary or it barely
  //-- moved since the last recorded point (below MIN_MOVEMENT_METERS).
  if (s_has_last_coord)
  {
    double distance_since_last_m =
        calculate_distance_m(s_last_lat, s_last_lon, gps->latitude_deg, gps->longitude_deg);

    bool insufficient_movement = distance_since_last_m < MIN_MOVEMENT_METERS;

    ESP_LOGD(TAG, "GPS point check: speed=%.2fkm/h last-speed=%.2fkm/h since-last=%.2fm",
             gps->speed_kmh, s_last_written_speed_kmh, distance_since_last_m);

    if (stationary || insufficient_movement)
    {
      s_last_sequence = gps->sequence;
      return ESP_OK;
    }

    ESP_LOGI(TAG, "Recording GPS point: sequence=%u trip-distance=%.2f since-last=%.2fm",
             gps->sequence, trip_distance_m, distance_since_last_m);
  }

  s_trip_distance_m = fmaxf(0.0f, trip_distance_m);

  //-- GPX/CSV position timestamps must be the raw GPS Zulu (UTC) time, not the
  //-- local time used only for the trip file names.
  char date[16];
  char time[24];
  char gpx_point[512];
  char csv_row[256];
  snprintf(date, sizeof(date), "%04u-%02u-%02u", gps->year, gps->month, gps->day);
  snprintf(time, sizeof(time), "%02u:%02u:%02uZ", gps->hour, gps->minute, gps->second);
  int gpx_written =
      snprintf(gpx_point, sizeof(gpx_point),
               "    <trkpt lat=\"%.7f\" "
               "lon=\"%.7f\"><ele>%.2f</ele><time>%sT%s</time><extensions><speed_kmh>%.3f</"
               "speed_kmh><course_deg>%.3f</course_deg><satellites>%u</"
               "satellites><distance_m>%.2f</distance_m></extensions></trkpt>\n",
               gps->latitude_deg, gps->longitude_deg, gps->altitude_m, date, time, gps->speed_kmh,
               gps->course_deg, gps->satellites, s_trip_distance_m);
  int csv_written = snprintf(csv_row, sizeof(csv_row), "%s,%s,%.7f,%.7f,%.2f,%.3f,%.3f,%u,%.2f\n",
                             date, time, gps->latitude_deg, gps->longitude_deg, gps->altitude_m,
                             gps->speed_kmh, gps->course_deg, gps->satellites, s_trip_distance_m);
  if (gpx_written < 0 || (size_t)gpx_written >= sizeof(gpx_point) || csv_written < 0 ||
      (size_t)csv_written >= sizeof(csv_row) ||
      write_text(s_trip_gpx_fd, s_trip_gpx_path, gpx_point) != ESP_OK ||
      write_text(s_trip_csv_fd, s_trip_csv_path, csv_row) != ESP_OK || fsync(s_trip_gpx_fd) != 0 ||
      fsync(s_trip_csv_fd) != 0)
  {
    ESP_LOGE(TAG, "Failed to write GPX/CSV GPS point");
    return ESP_FAIL;
  }
  s_last_sequence = gps->sequence;
  s_last_lat = gps->latitude_deg;
  s_last_lon = gps->longitude_deg;
  s_has_last_coord = true;
  s_last_written_speed_kmh = gps->speed_kmh;
  ++s_entry_count;
  return ESP_OK;
}

void sdcard_get_status(sdcard_status_t* status)
{
  if (!status)
    return;
  update_status(status);
}

uint16_t sdcard_get_trip_number(void)
{
  return s_current_trip_number;
}

void sdcard_get_active_trip_datetime(char* out, size_t out_size)
{
  if (!out || out_size == 0)
    return;
  out[0] = 0;

  const char* base = strrchr(s_trip_gpx_path, '/');
  base = base ? base + 1 : s_trip_gpx_path;
  const char* prefix = "trip-";
  size_t prefix_len = strlen(prefix);
  if (strncmp(base, prefix, prefix_len) != 0)
    return;

  const char* start = base + prefix_len;
  size_t length = strlen(start);
  if (length > 4 && strcmp(start + length - 4, ".gpx") == 0)
    length -= 4;
  //-- Drop the "O" (open) marker so the displayed datetime matches the final
  //-- "EEYYMMDD-HHmmSS" filename once the trip file is closed.
  if (length > 0 && start[length - 1] == 'O')
    --length;
  if (length >= out_size)
    length = out_size - 1;
  memcpy(out, start, length);
  out[length] = 0;
}

uint32_t sdcard_get_entry_count(void)
{
  return s_entry_count;
}

size_t sdcard_list_trip_files(sdcard_trip_summary_t* out, size_t max_count)
{
  if (!out || max_count == 0 || !s_mounted)
  {
    return 0;
  }

  DIR* directory = opendir(SD_MOUNT_POINT);
  if (!directory)
  {
    ESP_LOGE(TAG, "Cannot open SD card directory for trip listing");
    return 0;
  }

  size_t count = 0;
  struct dirent* entry;
  while (count < max_count && (entry = readdir(directory)) != NULL)
  {
    size_t name_length = strlen(entry->d_name);
    if (!is_trip_filename(entry->d_name) || strcmp(entry->d_name + name_length - 4, ".gpx") != 0)
    {
      continue;
    }

    sdcard_trip_summary_t summary = {0};
    if (!parse_trip_filename(entry->d_name, &summary))
    {
      continue;
    }

    char path[sizeof(s_trip_gpx_path)];
    int written = snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
      continue;
    }

    if (s_trip_gpx_fd >= 0 && strcmp(path, s_trip_gpx_path) == 0)
    {
      summary.distance_m = s_trip_distance_m;
    }
    else
    {
      summary.distance_m = read_trip_gpx_last_distance(path);
    }

    snprintf(summary.base_name, sizeof(summary.base_name), "%.*s", (int)(name_length - 4),
             entry->d_name);

    out[count++] = summary;
  }
  closedir(directory);

  qsort(out, count, sizeof(sdcard_trip_summary_t), compare_trip_summary_desc);
  return count;
}

esp_err_t sdcard_get_trip_details(const char* base_name, sdcard_trip_details_t* out)
{
  if (!out)
  {
    return ESP_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));

  if (!base_name || !s_mounted)
  {
    return ESP_FAIL;
  }

  char path[80];
  int written = snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s.csv", base_name);
  if (written < 0 || (size_t)written >= sizeof(path))
  {
    return ESP_FAIL;
  }

  FILE* file = fopen(path, "r");
  if (!file)
  {
    return ESP_FAIL;
  }

  char line[256];
  //-- Skip the CSV header row.
  fgets(line, sizeof(line), file);

  bool have_first = false;
  bool have_altitude = false;
  struct tm first_tm = {0};
  struct tm last_tm = {0};
  float altitude_min = 0.0f;
  float altitude_max = 0.0f;
  float last_distance = 0.0f;

  while (fgets(line, sizeof(line), file))
  {
    char date_str[16];
    char time_str[16];
    double latitude, longitude;
    float altitude, speed, course, distance;
    unsigned satellites;
    if (sscanf(line, "%15[^,],%15[^,],%lf,%lf,%f,%f,%f,%u,%f", date_str, time_str, &latitude,
               &longitude, &altitude, &speed, &course, &satellites, &distance) != 9)
    {
      continue;
    }

    int year, month, day, hour, minute, second;
    //-- time_str has a trailing "Z" (e.g. "14:30:05Z"); %d for seconds stops
    //-- at the first non-digit character so the "Z" is simply ignored.
    if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) != 3 ||
        sscanf(time_str, "%d:%d:%d", &hour, &minute, &second) != 3)
    {
      continue;
    }

    struct tm row_tm = {0};
    row_tm.tm_year = year - 1900;
    row_tm.tm_mon = month - 1;
    row_tm.tm_mday = day;
    row_tm.tm_hour = hour;
    row_tm.tm_min = minute;
    row_tm.tm_sec = second;

    if (!have_first)
    {
      first_tm = row_tm;
      have_first = true;
    }
    last_tm = row_tm;
    last_distance = distance;

    if (!have_altitude)
    {
      altitude_min = altitude;
      altitude_max = altitude;
      have_altitude = true;
    }
    else
    {
      if (altitude < altitude_min)
        altitude_min = altitude;
      if (altitude > altitude_max)
        altitude_max = altitude;
    }
  }
  fclose(file);

  if (!have_first)
  {
    return ESP_FAIL;
  }

  time_t first_time = mktime(&first_tm);
  time_t last_time = mktime(&last_tm);
  int64_t duration_s = (int64_t)last_time - (int64_t)first_time;
  if (duration_s < 0)
  {
    duration_s = 0;
  }

  out->distance_m = last_distance;
  out->duration_s = (uint32_t)duration_s;
  out->avg_speed_kmh =
      duration_s > 0 ? (last_distance / 1000.0f) / ((float)duration_s / 3600.0f) : 0.0f;
  out->altitude_diff_m = have_altitude ? (altitude_max - altitude_min) : 0.0f;
  out->end_hour = (uint8_t)last_tm.tm_hour;
  out->end_minute = (uint8_t)last_tm.tm_min;
  out->valid = true;
  return ESP_OK;
}

esp_err_t sdcard_finish(void)
{
  esp_err_t result = ESP_OK;
  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    if (close_trip_files() != ESP_OK)
    {
      result = ESP_FAIL;
    }
  }
  if (s_mounted)
  {
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (result == ESP_OK)
      result = err;
    s_card = NULL;
    s_mounted = false;
  }
  return result;
}
