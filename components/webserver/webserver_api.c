#include "webserver_internal.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"

#include "sdcard.h"
#include "webserver.h"

static const char* TAG = "webserver_api";

//-- Chunk size used when streaming a file download over the WebSocket.
#define WEBSERVER_WS_CHUNK_SIZE 2048

//-- The single WebSocket file-manager connection. Only one client may ever
//-- be "active"; a newer handshake takes over from an older one.
static httpd_handle_t s_ws_server;
static int s_active_fd = -1;
//-- Remembered for the disconnect log line, since the socket is no longer
//-- queryable by the time webserver_ws_on_session_close() runs.
static char s_active_ip[46] = "";

//-- Formats the peer address of an open socket as a plain IP string.
static void format_peer_ip(int fd, char* out, size_t out_size)
{
  out[0] = '\0';
  struct sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);
  if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) != 0)
  {
    return;
  }
  if (addr.ss_family == AF_INET)
  {
    inet_ntop(AF_INET, &((struct sockaddr_in*)&addr)->sin_addr, out, out_size);
  }
  else if (addr.ss_family == AF_INET6)
  {
    inet_ntop(AF_INET6, &((struct sockaddr_in6*)&addr)->sin6_addr, out, out_size);
  }
}

//-- Persisted across the separate WebSocket frames of one client's upload,
//-- since only one client (and therefore one upload) can ever be in flight.
typedef struct
{
  bool active;
  int fd;
  FILE* file;
  char path[320];
  char name[64];
} ws_upload_state_t;
static ws_upload_state_t s_upload;

//-- Resolves a JSON "store" field ("sd" or "fs") to its VFS mount point.
static bool resolve_store_base(cJSON* msg, const char** base_path)
{
  cJSON* store_item = cJSON_GetObjectItemCaseSensitive(msg, "store");
  if (!cJSON_IsString(store_item))
  {
    return false;
  }
  if (strcmp(store_item->valuestring, "sd") == 0)
  {
    *base_path = SDCARD_MOUNT_POINT;
    return true;
  }
  if (strcmp(store_item->valuestring, "fs") == 0)
  {
    *base_path = WEBSERVER_LITTLEFS_MOUNT_POINT;
    return true;
  }
  return false;
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

//-- Maximum number of directory entries build_files_json() will sort and report.
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

static cJSON* build_files_json(const char* base_path)
{
  cJSON* array = cJSON_CreateArray();

  DIR* dir = opendir(base_path);
  if (dir == NULL)
  {
    return array;
  }

  webserver_file_entry_t* entries =
      calloc(WEBSERVER_API_MAX_LISTED_FILES, sizeof(webserver_file_entry_t));
  if (entries == NULL)
  {
    closedir(dir);
    return array;
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

  return array;
}

static void send_json(httpd_req_t* req, cJSON* obj)
{
  char* text = cJSON_PrintUnformatted(obj);
  if (text == NULL)
  {
    return;
  }
  httpd_ws_frame_t frame = {0};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t*)text;
  frame.len = strlen(text);
  httpd_ws_send_frame(req, &frame);
  free(text);
}

static void send_error(httpd_req_t* req, const char* message)
{
  cJSON* obj = cJSON_CreateObject();
  cJSON_AddStringToObject(obj, "type", "error");
  cJSON_AddStringToObject(obj, "message", message);
  send_json(req, obj);
  cJSON_Delete(obj);
}

//-- Discards any in-flight upload; used both on abnormal close and when a
//-- new upload_start overrides one that was never finished with upload_end.
static void abort_upload(void)
{
  if (!s_upload.active)
  {
    return;
  }
  fclose(s_upload.file);
  remove(s_upload.path);
  memset(&s_upload, 0, sizeof(s_upload));
}

static void handle_msg_list(httpd_req_t* req, cJSON* msg)
{
  const char* base_path;
  if (!resolve_store_base(msg, &base_path))
  {
    send_error(req, "Invalid or missing 'store'");
    return;
  }

  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "type", "files");
  cJSON_AddStringToObject(response, "store",
                          cJSON_GetObjectItemCaseSensitive(msg, "store")->valuestring);
  cJSON_AddItemToObject(response, "files", build_files_json(base_path));
  send_json(req, response);
  cJSON_Delete(response);
}

static void handle_msg_delete(httpd_req_t* req, cJSON* msg)
{
  const char* base_path;
  cJSON* name_item = cJSON_GetObjectItemCaseSensitive(msg, "name");
  if (!resolve_store_base(msg, &base_path) || !cJSON_IsString(name_item) ||
      !file_name_is_valid(name_item->valuestring))
  {
    send_error(req, "Invalid request parameters");
    return;
  }
  const char* name = name_item->valuestring;

  bool ok = false;
  if (!is_protected_littlefs_file(base_path, name))
  {
    char full_path[320];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);
    ok = (remove(full_path) == 0);
    if (ok)
    {
      ESP_LOGI(TAG, "Deleted %s from %s", name, base_path);
    }
  }

  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "type", "delete_ack");
  cJSON_AddStringToObject(response, "name", name);
  cJSON_AddBoolToObject(response, "ok", ok);
  send_json(req, response);
  cJSON_Delete(response);
}

static void handle_msg_download(httpd_req_t* req, cJSON* msg)
{
  const char* base_path;
  cJSON* name_item = cJSON_GetObjectItemCaseSensitive(msg, "name");
  if (!resolve_store_base(msg, &base_path) || !cJSON_IsString(name_item) ||
      !file_name_is_valid(name_item->valuestring))
  {
    send_error(req, "Invalid request parameters");
    return;
  }
  const char* name = name_item->valuestring;

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  struct stat file_stat;
  if (stat(full_path, &file_stat) != 0)
  {
    send_error(req, "File not found");
    return;
  }

  FILE* file = fopen(full_path, "rb");
  if (file == NULL)
  {
    send_error(req, "File not found");
    return;
  }

  cJSON* start = cJSON_CreateObject();
  cJSON_AddStringToObject(start, "type", "download_start");
  cJSON_AddStringToObject(start, "name", name);
  cJSON_AddNumberToObject(start, "size", (double)file_stat.st_size);
  send_json(req, start);
  cJSON_Delete(start);

  char buffer[WEBSERVER_WS_CHUNK_SIZE];
  size_t read_bytes;
  bool failed = false;
  while (!failed && (read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
  {
    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = (uint8_t*)buffer;
    frame.len = read_bytes;
    if (httpd_ws_send_frame(req, &frame) != ESP_OK)
    {
      failed = true;
    }
  }
  fclose(file);

  cJSON* end = cJSON_CreateObject();
  if (failed)
  {
    cJSON_AddStringToObject(end, "type", "error");
    cJSON_AddStringToObject(end, "message", "Download interrupted");
  }
  else
  {
    cJSON_AddStringToObject(end, "type", "download_end");
    cJSON_AddStringToObject(end, "name", name);
  }
  send_json(req, end);
  cJSON_Delete(end);
}

static void handle_msg_upload_start(httpd_req_t* req, cJSON* msg)
{
  const char* base_path;
  cJSON* name_item = cJSON_GetObjectItemCaseSensitive(msg, "name");
  if (!resolve_store_base(msg, &base_path) || !cJSON_IsString(name_item) ||
      !file_name_is_valid(name_item->valuestring))
  {
    send_error(req, "Invalid request parameters");
    return;
  }
  const char* name = name_item->valuestring;

  if (is_protected_littlefs_file(base_path, name))
  {
    send_error(req, "This file cannot be replaced");
    return;
  }

  //-- Only one client (and therefore one upload) can ever be active; drop any
  //-- previous upload that was never finished with upload_end.
  abort_upload();

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  FILE* file = fopen(full_path, "wb");
  if (file == NULL)
  {
    send_error(req, "Unable to create file");
    return;
  }

  s_upload.active = true;
  s_upload.fd = httpd_req_to_sockfd(req);
  s_upload.file = file;
  snprintf(s_upload.path, sizeof(s_upload.path), "%s", full_path);
  snprintf(s_upload.name, sizeof(s_upload.name), "%s", name);
}

static void handle_msg_upload_end(httpd_req_t* req, cJSON* msg)
{
  (void)msg;
  bool ok = false;
  char name[64] = "";
  if (s_upload.active && s_upload.fd == httpd_req_to_sockfd(req))
  {
    fclose(s_upload.file);
    snprintf(name, sizeof(name), "%s", s_upload.name);
    ESP_LOGI(TAG, "Uploaded %s to %s", s_upload.name, s_upload.path);
    ok = true;
    memset(&s_upload, 0, sizeof(s_upload));
  }

  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "type", "upload_ack");
  cJSON_AddStringToObject(response, "name", name);
  cJSON_AddBoolToObject(response, "ok", ok);
  send_json(req, response);
  cJSON_Delete(response);
}

static void dispatch_binary_message(httpd_req_t* req, const uint8_t* data, size_t len)
{
  if (!s_upload.active || s_upload.fd != httpd_req_to_sockfd(req))
  {
    ESP_LOGW(TAG, "Ignoring unexpected binary frame (%u bytes)", (unsigned)len);
    return;
  }
  if (len > 0)
  {
    fwrite(data, 1, len, s_upload.file);
  }
}

static void dispatch_text_message(httpd_req_t* req, const char* data, size_t len)
{
  cJSON* msg = cJSON_ParseWithLength(data, len);
  if (msg == NULL)
  {
    send_error(req, "Invalid JSON message");
    return;
  }

  cJSON* type_item = cJSON_GetObjectItemCaseSensitive(msg, "type");
  const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
  ESP_LOGI(TAG, "WS message '%s' from fd=%d", type, httpd_req_to_sockfd(req));

  if (strcmp(type, "list") == 0)
  {
    handle_msg_list(req, msg);
  }
  else if (strcmp(type, "delete") == 0)
  {
    handle_msg_delete(req, msg);
  }
  else if (strcmp(type, "download") == 0)
  {
    handle_msg_download(req, msg);
  }
  else if (strcmp(type, "upload_start") == 0)
  {
    handle_msg_upload_start(req, msg);
  }
  else if (strcmp(type, "upload_end") == 0)
  {
    handle_msg_upload_end(req, msg);
  }
  else
  {
    send_error(req, "Unknown message type");
  }

  cJSON_Delete(msg);
}

//-- Runs in the httpd task shortly after being queued from handle_ws_connect();
//-- notifies the previous client that it has been replaced, then closes it.
static void async_notify_takeover(void* arg)
{
  int fd = (int)(intptr_t)arg;
  static const char* payload = "{\"type\":\"taken_over\"}";
  httpd_ws_frame_t frame = {0};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t*)payload;
  frame.len = strlen(payload);
  httpd_ws_send_frame_async(s_ws_server, fd, &frame);
  httpd_sess_trigger_close(s_ws_server, fd);
}

//-- Enforces "only one active client" on the /ws connection. Must run from
//-- the first inbound data frame of a new fd, NOT from ws_post_handshake_cb:
//-- on this ESP-IDF version, sending a frame from that hook breaks the
//-- normal recv/response flow for the connection (confirmed the hard way in
//-- a prior project, see /useWebSockets.md section 5), so this project uses
//-- the same first-data-frame check that project settled on instead.
static void check_takeover(httpd_req_t* req)
{
  int fd = httpd_req_to_sockfd(req);
  if (fd == s_active_fd)
  {
    return;
  }

  char ip[46];
  format_peer_ip(fd, ip, sizeof(ip));

  if (s_active_fd >= 0)
  {
    ESP_LOGI(TAG, "%s taken over by %s (fd %d -> %d)", s_active_ip, ip, s_active_fd, fd);
    httpd_queue_work(s_ws_server, async_notify_takeover, (void*)(intptr_t)s_active_fd);
  }
  else
  {
    ESP_LOGI(TAG, "%s connected (fd=%d)", ip, fd);
  }
  s_active_fd = fd;
  snprintf(s_active_ip, sizeof(s_active_ip), "%s", ip);

  cJSON* hello = cJSON_CreateObject();
  cJSON_AddStringToObject(hello, "type", "hello");
  send_json(req, hello);
  cJSON_Delete(hello);
}

static esp_err_t ws_handler(httpd_req_t* req)
{
  int fd = httpd_req_to_sockfd(req);
  check_takeover(req);

  httpd_ws_frame_t ws_pkt = {0};
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK)
  {
    ESP_LOGW(TAG, "httpd_ws_recv_frame failed to get frame len (fd=%d): %s", fd,
             esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGI(TAG, "WS frame type=%d len=%u (fd=%d)", ws_pkt.type, (unsigned)ws_pkt.len, fd);

  uint8_t* buf = NULL;
  if (ws_pkt.len)
  {
    buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL)
    {
      return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK)
    {
      ESP_LOGW(TAG, "httpd_ws_recv_frame failed (fd=%d): %s", fd, esp_err_to_name(ret));
      free(buf);
      return ret;
    }
  }

  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
  {
    dispatch_text_message(req, (const char*)buf, ws_pkt.len);
  }
  else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY)
  {
    dispatch_binary_message(req, buf, ws_pkt.len);
  }

  free(buf);
  return ESP_OK;
}

esp_err_t webserver_api_register(httpd_handle_t server)
{
  s_ws_server = server;
  s_active_fd = -1;
  s_active_ip[0] = '\0';
  memset(&s_upload, 0, sizeof(s_upload));

  //-- Deliberately no .ws_post_handshake_cb here: esp_http_server completes
  //-- the WS opening handshake (sending the 101 response) internally and
  //-- never calls .handler for that initial GET (see httpd_uri.c: "If the
  //-- request is websocket handshake, then do not call the uri->handler"),
  //-- but sending a frame from .ws_post_handshake_cb was found to break the
  //-- normal recv/response flow on this IDF version (see /useWebSockets.md
  //-- section 5), so "hello"/takeover instead runs from check_takeover(),
  //-- called on every inbound WS data frame from ws_handler().
  httpd_uri_t ws_uri = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = ws_handler,
      .is_websocket = true,
  };
  return httpd_register_uri_handler(server, &ws_uri);
}

void webserver_ws_on_session_close(httpd_handle_t hd, int sockfd)
{
  (void)hd;
  if (sockfd == s_active_fd)
  {
    ESP_LOGI(TAG, "%s disconnected (fd=%d)", s_active_ip, sockfd);
    s_active_fd = -1;
    s_active_ip[0] = '\0';
  }
  else
  {
    ESP_LOGD(TAG, "Non-active session closed (fd=%d)", sockfd);
  }
  if (s_upload.active && s_upload.fd == sockfd)
  {
    ESP_LOGW(TAG, "Upload aborted by session close (fd=%d)", sockfd);
    abort_upload();
  }
}
