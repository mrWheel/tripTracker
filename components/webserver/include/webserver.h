#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

//-- Mount point for the LittleFS partition holding the GUI assets.
#define WEBSERVER_LITTLEFS_MOUNT_POINT "/littlefs"

//-- WiFi state as observed by webserver_start(): off, station connected to an
//-- AP, or fallen back to the device's own captive-portal AP.
typedef enum
{
  WEBSERVER_WIFI_OFF,
  WEBSERVER_WIFI_CONNECTING,
  WEBSERVER_WIFI_STA_CONNECTED,
  WEBSERVER_WIFI_AP_MODE,
} webserver_wifi_status_t;

//-- Mounts LittleFS. Must be called once at boot before any other webserver_* call.
esp_err_t webserver_init(void);

//-- Status line severity for the optional on-screen [Start Webserver] log.
typedef enum
{
  WEBSERVER_LOG_INFO,
  WEBSERVER_LOG_ERROR,
  WEBSERVER_LOG_SUCCESS,
} webserver_log_level_t;
//-- Optional callback receiving short, user-facing status lines (e.g.
//-- "Connecting to known AP", "Starting Webserver") in addition to the
//-- normal ESP_LOGI trace.
typedef void (*webserver_status_log_fn_t)(const char* message, webserver_log_level_t level);
void webserver_set_status_log(webserver_status_log_fn_t fn);

//-- One-shot boot check: attempts to connect with stored WiFi credentials
//-- (falling back to the captive portal only for the duration of the attempt),
//-- then always turns WiFi back off. Does not start the file-manager server.
esp_err_t webserver_check_wifi_credentials(void);

//-- Starts WiFi (station, falling back to the captive portal if needed) and
//-- starts the file-manager HTTP server once a station connection is up.
//-- Safe to call repeatedly; a no-op while already started.
esp_err_t webserver_start(void);

//-- Stops the file-manager HTTP server and turns WiFi back off.
//-- Safe to call repeatedly; a no-op while already stopped.
esp_err_t webserver_stop(void);

//-- Current WiFi state; only meaningful while webserver_start() is active.
webserver_wifi_status_t webserver_get_wifi_status(void);

//-- Returns the connected SSID and assigned IPv4 address for display.
void webserver_get_wifi_display_info(char* ssid, size_t ssid_size, char* ip_address,
                                     size_t ip_address_size);
