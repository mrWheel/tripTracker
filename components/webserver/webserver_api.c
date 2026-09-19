#include "webserver_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"

#include "sdcard.h"
#include "webserver.h"

static const char* TAG = "webserver_api";

//-- Resolves the "store" query parameter ("sd" or "fs") to its VFS mount point.
static esp_err_t resolve_store_base(httpd_req_t* req, char* store_value, size_t store_value_size,
                                    const char** base_path)
{
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
  {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(query, "store", store_value, store_value_size) != ESP_OK)
  {
    return ESP_FAIL;
  }

  if (strcmp(store_value, "sd") == 0)
  {
    *base_path = SDCARD_MOUNT_POINT;
    return ESP_OK;
  }
  if (strcmp(store_value, "fs") == 0)
  {
    *base_path = WEBSERVER_LITTLEFS_MOUNT_POINT;
    return ESP_OK;
  }
  return ESP_FAIL;
}

//-- Rejects empty names and any attempt to escape the store's root directory.
static bool file_name_is_valid(const char* name)
{
  if (name[0] == '\0')
  {
    return false;
  }
  if (strchr(name, '/') != NULL)
  {
    return false;
  }
  if (strstr(name, "..") != NULL)
  {
    return false;
  }
  return true;
}

//-- The GUI itself is served from these LittleFS files; they must never be deletable.
static bool is_protected_littlefs_file(const char* base_path, const char* name)
{
  static const char* protected_names[] = {"style.css", "index.html", "app.js"};
  if (strcmp(base_path, WEBSERVER_LITTLEFS_MOUNT_POINT) != 0)
  {
    return false;
  }
  for (size_t i = 0; i < sizeof(protected_names) / sizeof(protected_names[0]); i++)
  {
    if (strcmp(name, protected_names[i]) == 0)
    {
      return true;
    }
  }
  return false;
}

//-- Maximum number of directory entries handle_list() will sort and report.
#define WEBSERVER_API_MAX_LISTED_FILES 256

typedef struct
{
  char name[64];
  long size;
  time_t mtime;
  bool has_trip_data;
  float distance_m;
  float avg_speed_kmh;
} webserver_file_entry_t;

//-- FAT timestamps depend on a system clock that this device never syncs from
//-- GPS, so mtime is unreliable. Trip filenames are zero-padded
//-- "trip-EEYYMMDD-HHmmSS", so a plain reverse filename comparison already
//-- sorts trip files newest first; other filenames just sort alphabetically.
static int compare_file_entries_newest_first(const void* a, const void* b)
{
  const webserver_file_entry_t* entry_a = (const webserver_file_entry_t*)a;
  const webserver_file_entry_t* entry_b = (const webserver_file_entry_t*)b;
  return strcmp(entry_b->name, entry_a->name);
}

//-- True when name ends with suffix, case-sensitive.
static bool has_suffix(const char* name, const char* suffix)
{
  size_t name_len = strlen(name);
  size_t suffix_len = strlen(suffix);
  if (suffix_len > name_len)
  {
    return false;
  }
  return strcmp(name + (name_len - suffix_len), suffix) == 0;
}

static bool read_trip_web_summary(const char* path, float* distance_m, float* avg_speed_kmh)
{
  FILE* file = fopen(path, "r");
  if (!file)
  {
    return false;
  }

  char line[512];
  char first_time[32] = {0};
  char last_time[32] = {0};
  float last_distance = 0.0f;
  bool found_trackpoint = false;
  while (fgets(line, sizeof(line), file))
  {
    char* time_start = strstr(line, "<time>");
    char* time_end = time_start ? strstr(time_start, "</time>") : NULL;
    char* distance_start = strstr(line, "<distance_m>");
    if (!time_start || !time_end || !distance_start)
    {
      continue;
    }

    time_start += strlen("<time>");
    size_t time_length = (size_t)(time_end - time_start);
    if (time_length >= sizeof(first_time))
    {
      continue;
    }

    char* distance_end;
    float distance = strtof(distance_start + strlen("<distance_m>"), &distance_end);
    if (distance_end == distance_start + strlen("<distance_m>"))
    {
      continue;
    }

    if (!found_trackpoint)
    {
      memcpy(first_time, time_start, time_length);
      first_time[time_length] = '\0';
      found_trackpoint = true;
    }
    memcpy(last_time, time_start, time_length);
    last_time[time_length] = '\0';
    last_distance = distance;
  }
  fclose(file);

  if (!found_trackpoint)
  {
    return false;
  }

  struct tm first_timestamp = {0};
  struct tm last_timestamp = {0};
  if (sscanf(first_time, "%d-%d-%dT%d:%d:%dZ", &first_timestamp.tm_year, &first_timestamp.tm_mon,
             &first_timestamp.tm_mday, &first_timestamp.tm_hour, &first_timestamp.tm_min,
             &first_timestamp.tm_sec) != 6 ||
      sscanf(last_time, "%d-%d-%dT%d:%d:%dZ", &last_timestamp.tm_year, &last_timestamp.tm_mon,
             &last_timestamp.tm_mday, &last_timestamp.tm_hour, &last_timestamp.tm_min,
             &last_timestamp.tm_sec) != 6)
  {
    return false;
  }
  first_timestamp.tm_year -= 1900;
  first_timestamp.tm_mon -= 1;
  last_timestamp.tm_year -= 1900;
  last_timestamp.tm_mon -= 1;
  int64_t duration_s = (int64_t)mktime(&last_timestamp) - (int64_t)mktime(&first_timestamp);
  if (duration_s < 0)
  {
    duration_s = 0;
  }

  *distance_m = last_distance;
  *avg_speed_kmh =
      duration_s > 0 ? (last_distance / 1000.0f) / ((float)duration_s / 3600.0f) : 0.0f;
  return true;
}

static esp_err_t handle_list(httpd_req_t* req)
{
  char store_value[8];
  const char* base_path;
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing 'store' parameter");
    return ESP_FAIL;
  }

  DIR* dir = opendir(base_path);
  if (dir == NULL)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to open storage directory");
    return ESP_FAIL;
  }

  webserver_file_entry_t* entries =
      calloc(WEBSERVER_API_MAX_LISTED_FILES, sizeof(webserver_file_entry_t));
  if (entries == NULL)
  {
    closedir(dir);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }

  size_t entry_count = 0;
  struct dirent* entry;
  char full_path[320];
  while ((entry = readdir(dir)) != NULL && entry_count < WEBSERVER_API_MAX_LISTED_FILES)
  {
    if (entry->d_name[0] == '.')
    {
      continue;
    }

    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
    struct stat file_stat;
    if (stat(full_path, &file_stat) != 0)
    {
      continue;
    }

    webserver_file_entry_t* out = &entries[entry_count];
    snprintf(out->name, sizeof(out->name), "%.63s", entry->d_name);
    out->size = (long)file_stat.st_size;
    out->mtime = file_stat.st_mtime;

    //-- Trip distance/average speed are only available for SD-card GPX trip files.
    if (strcmp(base_path, SDCARD_MOUNT_POINT) == 0 && has_suffix(out->name, ".gpx"))
    {
      if (read_trip_web_summary(full_path, &out->distance_m, &out->avg_speed_kmh))
      {
        out->has_trip_data = true;
      }
    }

    entry_count++;
  }
  closedir(dir);

  qsort(entries, entry_count, sizeof(webserver_file_entry_t), compare_file_entries_newest_first);

  cJSON* array = cJSON_CreateArray();
  for (size_t i = 0; i < entry_count; i++)
  {
    webserver_file_entry_t* out = &entries[i];
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", out->name);
    cJSON_AddNumberToObject(item, "size", out->size);
    if (out->has_trip_data)
    {
      cJSON_AddNumberToObject(item, "distance_m", out->distance_m);
      cJSON_AddNumberToObject(item, "avg_speed_kmh", out->avg_speed_kmh);
    }
    cJSON_AddItemToArray(array, item);
  }
  free(entries);

  char* json_text = cJSON_PrintUnformatted(array);
  cJSON_Delete(array);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, json_text);
  free(json_text);
  return ESP_OK;
}

static esp_err_t handle_download(httpd_req_t* req)
{
  char store_value[8];
  const char* base_path;
  char name[64];

  char query[128];
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
      !file_name_is_valid(name))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request parameters");
    return ESP_FAIL;
  }

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  FILE* file = fopen(full_path, "rb");
  if (file == NULL)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  char disposition[96];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);

  char buffer[512];
  size_t read_bytes;
  esp_err_t result = ESP_OK;
  while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
  {
    if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK)
    {
      result = ESP_FAIL;
      break;
    }
  }
  fclose(file);

  if (result == ESP_OK)
  {
    httpd_resp_send_chunk(req, NULL, 0);
  }
  return result;
}

static esp_err_t handle_upload(httpd_req_t* req)
{
  char store_value[8];
  const char* base_path;
  char name[64];

  char query[128];
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
      !file_name_is_valid(name))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request parameters");
    return ESP_FAIL;
  }

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  FILE* file = fopen(full_path, "wb");
  if (file == NULL)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to create file");
    return ESP_FAIL;
  }

  char buffer[512];
  int remaining = req->content_len;
  while (remaining > 0)
  {
    int to_read = remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer);
    int received = httpd_req_recv(req, buffer, to_read);
    if (received <= 0)
    {
      fclose(file);
      remove(full_path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload aborted");
      return ESP_FAIL;
    }
    fwrite(buffer, 1, received, file);
    remaining -= received;
  }
  fclose(file);

  ESP_LOGI(TAG, "Uploaded %s to %s (%d bytes)", name, base_path, req->content_len);
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static esp_err_t handle_delete(httpd_req_t* req)
{
  char store_value[8];
  const char* base_path;
  char name[64];

  char query[128];
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
      !file_name_is_valid(name))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request parameters");
    return ESP_FAIL;
  }

  if (is_protected_littlefs_file(base_path, name))
  {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "This file cannot be deleted");
    return ESP_FAIL;
  }

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  if (remove(full_path) != 0)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Deleted %s from %s", name, base_path);
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

esp_err_t webserver_api_register(httpd_handle_t server)
{
  httpd_uri_t list_uri = {.uri = "/api/files", .method = HTTP_GET, .handler = handle_list};
  httpd_uri_t download_uri = {
      .uri = "/api/download", .method = HTTP_GET, .handler = handle_download};
  httpd_uri_t upload_uri = {.uri = "/api/upload", .method = HTTP_POST, .handler = handle_upload};
  httpd_uri_t delete_uri = {.uri = "/api/delete", .method = HTTP_DELETE, .handler = handle_delete};

  esp_err_t err;
  if ((err = httpd_register_uri_handler(server, &list_uri)) != ESP_OK)
    return err;
  if ((err = httpd_register_uri_handler(server, &download_uri)) != ESP_OK)
    return err;
  if ((err = httpd_register_uri_handler(server, &upload_uri)) != ESP_OK)
    return err;
  if ((err = httpd_register_uri_handler(server, &delete_uri)) != ESP_OK)
    return err;
  return ESP_OK;
}
