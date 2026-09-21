#include "webserver.h"

#include <stdarg.h>
#include <stdbool.h>

#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "wifi_provisioner.h"

#include "webserver_internal.h"

//-- mDNS hostname, reachable as "tripTracker.local" while WiFi is active.
#define WEBSERVER_HOSTNAME "tripTracker"

static const char* TAG = "webserver";
static httpd_handle_t s_server;
static bool s_network_active;
static bool s_mdns_active;
//-- Running count of accepted-but-not-yet-closed sockets, tracked via
//-- open_fn/close_fn below; logged on every change to make socket exhaustion
//-- (the CONFIG_LWIP_MAX_SOCKETS ceiling) visible before it happens.
static volatile int s_open_socket_count;

//-- Live connection state, tracked independently of wifi_prov_is_connected()
//-- (which is never cleared again once a STA connection later drops).
static volatile bool s_connecting;
static volatile bool s_sta_connected;
static volatile bool s_ap_mode;
static bool s_wifi_event_handlers_registered;
//-- Optional sink for short, user-facing status lines (see webserver_set_status_log()).
static webserver_status_log_fn_t s_status_log_fn;

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
    s_status_log_fn(message, WEBSERVER_LOG_INFO);
  }
}

//-- Same as report_status(), but flags the line as an error for on-screen
//-- highlighting (e.g. the known AP not being found).
static void report_error(const char* fmt, ...)
{
  char message[64];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  ESP_LOGW(TAG, "%s", message);
  if (s_status_log_fn)
  {
    s_status_log_fn(message, WEBSERVER_LOG_ERROR);
  }
}

//-- Same as report_status(), but flags the line for on-screen success/green
//-- highlighting (e.g. the SSID/IP that was just connected to).
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
    s_status_log_fn(message, WEBSERVER_LOG_SUCCESS);
  }
}

void webserver_set_status_log(webserver_status_log_fn_t fn)
{
  s_status_log_fn = fn;
}

static esp_err_t mount_littlefs(void)
{
  esp_vfs_littlefs_conf_t conf = {
      .base_path = WEBSERVER_LITTLEFS_MOUNT_POINT,
      .partition_label = "littlefs",
      .format_if_mount_failed = true,
      .dont_mount = false,
  };

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(err));
  }
  return err;
}

//-- httpd_config_t.open_fn hook: fires for EVERY accepted socket (each static
//-- GUI asset request and the /ws upgrade alike), before any request is
//-- parsed. This is the earliest point at which we can see a client actually
//-- reach accept() at all, and how close the system is to CONFIG_LWIP_MAX_SOCKETS.
static esp_err_t on_socket_open(httpd_handle_t hd, int sockfd)
{
  (void)hd;
  s_open_socket_count++;
  ESP_LOGI(TAG, "Socket accepted (fd=%d, open=%d)", sockfd, s_open_socket_count);
  return ESP_OK;
}

//-- httpd_config_t.close_fn hook: fires for every socket close, not just
//-- WebSocket sessions. Logs the running count, then forwards to the
//-- WebSocket-specific cleanup already implemented in webserver_api.c.
static void on_socket_close(httpd_handle_t hd, int sockfd)
{
  s_open_socket_count--;
  ESP_LOGI(TAG, "Socket closed (fd=%d, open=%d)", sockfd, s_open_socket_count);
  webserver_ws_on_session_close(hd, sockfd);
}

static void start_http_server(void)
{
  if (s_server != NULL)
  {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 16;
  //-- Raised from the default 7 now that CONFIG_LWIP_MAX_SOCKETS gives the
  //-- system enough headroom (mDNS/WiFi included) for this many client sockets.
  //-- httpd_start() itself reserves 3 sockets from CONFIG_LWIP_MAX_SOCKETS on
  //-- top of this value, so this + 3 must leave real headroom for the rest of
  //-- the system (WiFi/DHCP/ARP/etc.) — see CONFIG_LWIP_MAX_SOCKETS=22 in
  //-- sdkconfig.defaults.
  config.max_open_sockets = 10;
  //-- Without this, once max_open_sockets lingering connections pile up the
  //-- server can no longer accept() anything at all (not even a fresh /ws
  //-- reconnect); purging the least-recently-used one keeps it unstuck.
  config.lru_purge_enable = true;
  //-- Logs and counts every accepted socket, regardless of URI.
  config.open_fn = on_socket_open;
  //-- Logs+counts the close, then clears the single-client "active" state
  //-- (webserver_ws_on_session_close) even on abnormal disconnects.
  config.close_fn = on_socket_close;
  //-- Without SO_KEEPALIVE, lwIP only notices a WebSocket peer that vanished
  //-- without a clean close (WiFi drop, laptop sleep, ...) once it gives up
  //-- retransmitting the unacked handshake/data after CONFIG_LWIP_TCP_MAXRTX
  //-- attempts — observed on hardware as a ~300s stall before the socket is
  //-- reclaimed ("httpd_ws_get_frame_type: ... socket FD invalid"), during
  //-- which the GUI just sits dead. These settings detect it in ~15s instead.
  config.keep_alive_enable = true;
  config.keep_alive_idle = 5;
  config.keep_alive_interval = 3;
  config.keep_alive_count = 3;
  //-- Default (4096) proved too small: handle_msg_download()'s locals alone
  //-- (full_path[320] + buffer[2048]) plus cJSON and the SD/SPI driver call
  //-- chain underneath overflowed it, corrupting heap-adjacent structures and
  //-- crashing later inside the FreeRTOS scheduler on Download/Delete clicks.
  config.stack_size = 10240;

  s_open_socket_count = 0;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    s_server = NULL;
    return;
  }

  webserver_api_register(s_server);
  webserver_static_register(s_server);

  report_status("Starting Webserver");
  report_success("> Webserver active");
  ESP_LOGI(TAG, "max_open_sockets=%d, max_uri_handlers=%d", config.max_open_sockets,
           config.max_uri_handlers);
}

static void stop_http_server(void)
{
  if (s_server == NULL)
  {
    return;
  }
  httpd_stop(s_server);
  s_server = NULL;
  ESP_LOGI(TAG, "File-manager web server stopped");
}

static void on_wifi_connected(void)
{
  //-- Fires when the portal flow hands over a working STA connection.
  s_sta_connected = true;
  s_ap_mode = false;
  //-- ESP-IDF defaults to WIFI_PS_MIN_MODEM (visible in the boot log as
  //-- "wifi:pm start, type: 1"), which lets the radio sleep between beacons.
  //-- That sleep can delay/miss inbound packets on the long-lived /ws
  //-- connection this GUI depends on for seconds at a time, long enough for
  //-- the browser to consider it dead and fire onclose (observed on hardware
  //-- as the WS connection dying every few seconds while the ESP32-side
  //-- session still thinks it's fine). A single-client file-manager GUI has
  //-- no power budget worth saving here, so disable power save outright.
  esp_wifi_set_ps(WIFI_PS_NONE);
  char ssid[33] = "";
  char ip_address[16] = "";
  webserver_get_wifi_display_info(ssid, sizeof(ssid), ip_address, sizeof(ip_address));
  report_status("Connecting to:");
  report_success("> %s", ssid);
  report_success("> %s", ip_address);
  start_http_server();
}

static void on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
  {
    s_sta_connected = false;
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
  {
    s_sta_connected = true;
    s_ap_mode = false;
  }
}

static void register_wifi_event_handlers(void)
{
  if (s_wifi_event_handlers_registered)
  {
    return;
  }
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
  s_wifi_event_handlers_registered = true;
}

static void unregister_wifi_event_handlers(void)
{
  if (!s_wifi_event_handlers_registered)
  {
    return;
  }
  esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event);
  s_wifi_event_handlers_registered = false;
}

static void start_mdns(void)
{
  if (s_mdns_active)
  {
    return;
  }

  esp_err_t err = mdns_init();
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start mDNS: %s", esp_err_to_name(err));
    return;
  }

  mdns_hostname_set(WEBSERVER_HOSTNAME);
  mdns_instance_name_set(WEBSERVER_HOSTNAME);
  mdns_service_add(WEBSERVER_HOSTNAME, "_http", "_tcp", 80, NULL, 0);
  s_mdns_active = true;
  ESP_LOGI(TAG, "mDNS hostname set to %s.local", WEBSERVER_HOSTNAME);
}

static void stop_mdns(void)
{
  if (!s_mdns_active)
  {
    return;
  }
  mdns_free();
  s_mdns_active = false;
}

//-- Runs the (potentially slow, multi-second) connect-or-provision attempt on
//-- its own task so opening the System Menu never blocks the UI loop.
static void wifi_connect_task(void* arg)
{
  wifi_prov_config_t config = WIFI_PROV_DEFAULT_CONFIG();
  config.ap_ssid = WEBSERVER_HOSTNAME;
  config.on_connected = on_wifi_connected;

  report_status("Connecting to known AP");
  esp_err_t err = wifi_prov_start(&config);
  if (err == ESP_OK)
  {
    start_mdns();
    if (wifi_prov_is_connected())
    {
      s_sta_connected = true;
      start_http_server();
    }
    else
    {
      s_ap_mode = true;
      //-- The known AP could not be reached: this is exceptional, so guide
      //-- the user clearly on what to do next, one short line at a time.
      report_error("NOT FOUND!");
      report_status("Starting Captive Portal");
      report_status("Connect WiFi to:");
      report_status("%s", WEBSERVER_HOSTNAME);
      report_status("Browse to:");
      report_status("%s.local", WEBSERVER_HOSTNAME);
      report_status("or 192.168.1.4");
    }
  }
  else
  {
    ESP_LOGE(TAG, "Failed to start WiFi provisioning: %s", esp_err_to_name(err));
  }

  s_connecting = false;
  vTaskDelete(NULL);
}

esp_err_t webserver_init(void)
{
  return mount_littlefs();
}

esp_err_t webserver_check_wifi_credentials(void)
{
  ESP_LOGI(TAG, "Checking stored WiFi credentials");

  wifi_prov_config_t config = WIFI_PROV_DEFAULT_CONFIG();
  config.ap_ssid = WEBSERVER_HOSTNAME;
  esp_err_t err = wifi_prov_start(&config);
  if (wifi_prov_is_connected())
  {
    ESP_LOGI(TAG, "WiFi credentials OK, station connected");
  }
  else
  {
    ESP_LOGW(TAG, "WiFi station not connected (no/invalid credentials)");
  }

  wifi_prov_stop();
  return err;
}

esp_err_t webserver_start(void)
{
  if (s_network_active)
  {
    return ESP_OK;
  }

  s_sta_connected = false;
  s_ap_mode = false;
  s_connecting = true;
  s_network_active = true;

  wifi_prov_init();
  register_wifi_event_handlers();

  if (xTaskCreate(wifi_connect_task, "wifi_connect", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) !=
      pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create WiFi connect task");
    s_connecting = false;
    s_network_active = false;
    unregister_wifi_event_handlers();
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t webserver_stop(void)
{
  if (!s_network_active)
  {
    return ESP_OK;
  }

  //-- Let a still-running connect attempt finish before tearing WiFi down.
  while (s_connecting)
  {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  unregister_wifi_event_handlers();
  stop_http_server();
  stop_mdns();
  esp_err_t err = wifi_prov_stop();
  s_sta_connected = false;
  s_ap_mode = false;
  s_network_active = false;
  return err;
}

webserver_wifi_status_t webserver_get_wifi_status(void)
{
  if (!s_network_active)
  {
    return WEBSERVER_WIFI_OFF;
  }
  if (s_connecting)
  {
    return WEBSERVER_WIFI_CONNECTING;
  }
  if (s_sta_connected)
  {
    return WEBSERVER_WIFI_STA_CONNECTED;
  }
  if (s_ap_mode)
  {
    return WEBSERVER_WIFI_AP_MODE;
  }
  return WEBSERVER_WIFI_OFF;
}

void webserver_get_wifi_display_info(char* ssid, size_t ssid_size, char* ip_address,
                                     size_t ip_address_size)
{
  if (ssid && ssid_size > 0)
  {
    ssid[0] = '\0';
  }
  if (ip_address && ip_address_size > 0)
  {
    ip_address[0] = '\0';
  }

  if (ssid && ssid_size > 0)
  {
    wifi_config_t config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK)
    {
      snprintf(ssid, ssid_size, "%s", (char*)config.sta.ssid);
    }
  }

  if (ip_address && ip_address_size > 0)
  {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
    {
      snprintf(ip_address, ip_address_size, IPSTR, IP2STR(&ip_info.ip));
    }
  }
}
