#pragma once

#include "esp_http_server.h"

//-- Registers the single "/ws" WebSocket URI handler that carries the whole
//-- GUI protocol (presence/takeover plus list/upload/download/delete) on the given server.
esp_err_t webserver_api_register(httpd_handle_t server);

//-- Registers the static GUI file URI handlers (index.html, style.css, app.js) on the given server.
esp_err_t webserver_static_register(httpd_handle_t server);

//-- httpd_config_t.close_fn hook: clears the active-client state and aborts
//-- any in-flight upload when a WebSocket session closes for any reason.
void webserver_ws_on_session_close(httpd_handle_t hd, int sockfd);
