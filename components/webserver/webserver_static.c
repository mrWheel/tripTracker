#include "webserver_internal.h"

#include <stdio.h>

#include "esp_log.h"

#include "webserver.h"

static const char* TAG = "webserver_static";

typedef struct
{
  const char* uri;
  const char* file_name;
  const char* content_type;
} static_file_t;

static const static_file_t s_static_files[] = {
    {"/", "index.html", "text/html"},
    {"/index.html", "index.html", "text/html"},
    {"/style.css", "style.css", "text/css"},
    {"/app.js", "app.js", "application/javascript"},
};

static esp_err_t serve_static_file(httpd_req_t* req)
{
  const static_file_t* entry = (const static_file_t*)req->user_ctx;

  char path[64];
  snprintf(path, sizeof(path), "%s/%s", WEBSERVER_LITTLEFS_MOUNT_POINT, entry->file_name);

  FILE* file = fopen(path, "r");
  if (file == NULL)
  {
    ESP_LOGE(TAG, "Missing GUI asset: %s", path);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "GUI asset not found");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, entry->content_type);
  //-- Static assets are small and requested once per page load; closing the
  //-- socket right away (instead of HTTP keep-alive) frees it for the /ws
  //-- upgrade instead of it sitting idle in the small socket pool.
  httpd_resp_set_hdr(req, "Connection", "close");

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

esp_err_t webserver_static_register(httpd_handle_t server)
{
  for (size_t i = 0; i < sizeof(s_static_files) / sizeof(s_static_files[0]); i++)
  {
    httpd_uri_t uri = {
        .uri = s_static_files[i].uri,
        .method = HTTP_GET,
        .handler = serve_static_file,
        .user_ctx = (void*)&s_static_files[i],
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK)
    {
      return err;
    }
  }
  return ESP_OK;
}
