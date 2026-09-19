#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board.h"
#include "gps.h"
#include "lcd.h"
#include "sdcard.h"
#include "speedometer.h"
#include "webserver.h"

// — Program version string (keep manually updated with each release)
// — NEVER CHANGE THIS const char* NAME
// —             vvvvvvvvvvvvvv
static const char* PROG_VERSION = "v1.3.8";
// —             ^^^^^^^^^^^^^^
static const char* TAG = "tripTracker";

//-- Total system-menu items and how many are visible at once; must stay in
//-- sync with lcd.c's kMenuItems array and kVisibleMenuItems.
#define MENU_ITEM_COUNT 8
#define MENU_VISIBLE_ITEMS 6

//-- Cycle order for the [System Menu] > Display Blackout toggle, in minutes;
//-- 0 means the display never blacks out automatically.
static const uint8_t kBlackoutMinuteOptions[] = {1, 2, 4, 8, 0};
#define BLACKOUT_OPTION_COUNT (sizeof(kBlackoutMinuteOptions) / sizeof(kBlackoutMinuteOptions[0]))

//-- Rows visible at once in [LIST TRIPS]; must stay in sync with lcd.c's
//-- max_visible_rows for the list_trips_menu screen.
#define LIST_TRIPS_VISIBLE_ROWS 7

//-- Trip file pairs with a .gpx file smaller than this are deleted when
//-- [WiFi Menu] starts.
#define WIFI_MENU_MIN_TRIP_GPX_BYTES 5120
#define FOLLOWUP_SCREEN_TIMEOUT_US 60000000LL

static bool g_show_average = false;
static bool g_show_total = false;
static bool g_trip_mode = false;
static bool g_display_on = true;
static bool g_display_forced_off = false;
static bool g_system_menu = false;
static bool g_wifi_menu = false;
static bool g_list_trips_menu = false;
static bool g_trip_info_menu = false;
static bool g_format_confirm_menu = false;
static bool g_format_confirm_yes = false;
static uint8_t g_menu_selection = 0;
static uint8_t g_menu_scroll = 0;
static uint8_t g_blackout_minutes = 4;
static bool g_menu_action_active = false;
static bool g_menu_action_pending = false;
static uint8_t g_menu_action_selection = 0;
static int64_t g_menu_action_requested_us = 0;
static int64_t g_last_user_activity_us = 0;
static sdcard_status_t g_storage_status;
static lcd_trip_entry_t g_trip_entries[SDCARD_MAX_LISTED_TRIPS];
static char g_trip_base_names[SDCARD_MAX_LISTED_TRIPS][32];
static size_t g_trip_entry_count = 0;
static size_t g_list_trips_scroll = 0;
static size_t g_list_trips_selection = 0;
static lcd_trip_entry_t g_trip_info_entry;
static sdcard_trip_details_t g_trip_info_details;
//-- Scrolling status log shown on the [Start Webserver] screen.
static char g_wifi_log_lines[LCD_WIFI_LOG_MAX_LINES][LCD_WIFI_LOG_LINE_LEN];
static uint16_t g_wifi_log_colors[LCD_WIFI_LOG_MAX_LINES];
static uint8_t g_wifi_log_count;

//-- Non-blocking startup steps for [Start Webserver]: each step runs on its
//-- own main-loop iteration so the screen can redraw between them instead of
//-- freezing for however long the SD-card cleanup takes.
typedef enum
{
  WIFI_STARTUP_IDLE,
  WIFI_STARTUP_CLOSE_TRIP_FILES,
  WIFI_STARTUP_REMOVE_SMALL,
  WIFI_STARTUP_REMOVE_UNDERSIZED,
  WIFI_STARTUP_START_WEBSERVER,
} wifi_startup_state_t;
static wifi_startup_state_t g_wifi_startup_state = WIFI_STARTUP_IDLE;

//-- Appends a status line (with its display color) to the [Start Webserver]
//-- screen log; sdcard.c/webserver.c already ESP_LOGI/ESP_LOGW it themselves.
static void wifi_log_append(const char* line, uint16_t color)
{
  if (g_wifi_log_count < LCD_WIFI_LOG_MAX_LINES)
  {
    snprintf(g_wifi_log_lines[g_wifi_log_count], LCD_WIFI_LOG_LINE_LEN, "%s", line);
    g_wifi_log_colors[g_wifi_log_count] = color;
    ++g_wifi_log_count;
  }
  else
  {
    memmove(g_wifi_log_lines[0], g_wifi_log_lines[1],
            sizeof(g_wifi_log_lines) - sizeof(g_wifi_log_lines[0]));
    memmove(g_wifi_log_colors, g_wifi_log_colors + 1,
            sizeof(g_wifi_log_colors) - sizeof(g_wifi_log_colors[0]));
    snprintf(g_wifi_log_lines[LCD_WIFI_LOG_MAX_LINES - 1], LCD_WIFI_LOG_LINE_LEN, "%s", line);
    g_wifi_log_colors[LCD_WIFI_LOG_MAX_LINES - 1] = color;
  }
  //-- Deliberately no lcd_force_redraw() here: that would re-run the
  //-- expensive full-screen draw_static_frame() clear on every single log
  //-- line, causing a visible flash/restart. lcd_render() already redraws
  //-- this screen on its own whenever wifi_log_count/lines/colors change.
}

//-- sdcard.c's status-log callback: maps its level enum to a display color.
static void sdcard_wifi_log(const char* message, sdcard_log_level_t level)
{
  wifi_log_append(message, level == SDCARD_LOG_SUCCESS ? LCD_COLOR_GREEN : LCD_COLOR_YELLOW);
}

//-- webserver.c's status-log callback: maps its level enum to a display color.
static void webserver_wifi_log(const char* message, webserver_log_level_t level)
{
  uint16_t color = LCD_COLOR_YELLOW;
  if (level == WEBSERVER_LOG_ERROR)
  {
    color = LCD_COLOR_RED;
  }
  else if (level == WEBSERVER_LOG_SUCCESS)
  {
    color = LCD_COLOR_GREEN;
  }
  wifi_log_append(message, color);
}

static const char* menu_option_name(uint8_t selection)
{
  switch (selection)
  {
  case 0:
    return "New Trip file and reset Trip";
  case 1:
    return "Display Blackout";
  case 2:
    return "Start Webserver";
  case 3:
    return "Show Used & Free on SD";
  case 4:
    return "List Trip Files";
  case 5:
    return "Format SDcard";
  case 6:
    return "Reset Tracker";
  case 7:
    return "Exit";
  default:
    return "Unknown";
  }
}

//-- Formats the current Display Blackout setting as "Never" or "<n> Min".
static void blackout_setting_label(char* buffer, size_t buffer_len)
{
  if (g_blackout_minutes == 0)
  {
    snprintf(buffer, buffer_len, "Never");
  }
  else
  {
    snprintf(buffer, buffer_len, "%u Min", g_blackout_minutes);
  }
}

//-- Advances the Display Blackout setting to the next value in
//-- kBlackoutMinuteOptions, wrapping back to the first entry.
static void advance_blackout_setting(void)
{
  size_t current_index = 0;
  for (size_t i = 0; i < BLACKOUT_OPTION_COUNT; ++i)
  {
    if (kBlackoutMinuteOptions[i] == g_blackout_minutes)
    {
      current_index = i;
      break;
    }
  }
  g_blackout_minutes = kBlackoutMinuteOptions[(current_index + 1) % BLACKOUT_OPTION_COUNT];
}

static void refresh_trip_list(void)
{
  sdcard_trip_summary_t summaries[SDCARD_MAX_LISTED_TRIPS];
  size_t count = sdcard_list_trip_files(summaries, SDCARD_MAX_LISTED_TRIPS);
  for (size_t i = 0; i < count; ++i)
  {
    g_trip_entries[i].year = summaries[i].year;
    g_trip_entries[i].month = summaries[i].month;
    g_trip_entries[i].day = summaries[i].day;
    g_trip_entries[i].hour = summaries[i].hour;
    g_trip_entries[i].minute = summaries[i].minute;
    g_trip_entries[i].second = summaries[i].second;
    g_trip_entries[i].distance_m = summaries[i].distance_m;
    snprintf(g_trip_base_names[i], sizeof(g_trip_base_names[i]), "%s", summaries[i].base_name);
  }
  g_trip_entry_count = count;
  g_list_trips_scroll = 0;
  g_list_trips_selection = 0;
}

static void log_menu_cursor(board_button_t button)
{
  ESP_LOGI("board", "Button %s => [%s]", button == BOARD_BUTTON_A ? "A (LEFT)" : "C (RIGHT)",
           menu_option_name(g_menu_selection));
}

//-- Turns the user-selected Display Blackout minutes into an effective
//-- timeout in seconds, applying the low-battery reductions.
static uint32_t effective_blackout_seconds(uint8_t setting_minutes, int battery_pct, bool charging)
{
  if (charging)
  {
    return 0; // no automatic timeout while charging/USB powered
  }
  if (setting_minutes == 0)
  {
    //-- "Never" still blacks out after 1 minute once the battery drops below 25%.
    if (battery_pct < 25)
    {
      return 60;
    }
    return 0;
  }
  uint32_t seconds = (uint32_t)setting_minutes * 60;
  if (battery_pct < 50)
  {
    seconds /= 2;
  }
  return seconds;
}

static void turn_display_on(void)
{
  if (!g_display_on)
  {
    lcd_set_backlight(true);
    g_display_on = true;
    lcd_force_redraw();
  }
  g_last_user_activity_us = esp_timer_get_time();
}

static void turn_display_off(bool forced)
{
  lcd_set_backlight(false);
  g_display_on = false;
  g_display_forced_off = forced;
}

static bool followup_screen_is_active(void)
{
  return g_system_menu || g_menu_action_active || g_list_trips_menu || g_trip_info_menu ||
         g_format_confirm_menu;
}

static void return_to_main_screen(void)
{
  g_system_menu = false;
  g_menu_action_active = false;
  g_menu_action_pending = false;
  g_list_trips_menu = false;
  g_trip_info_menu = false;
  g_format_confirm_menu = false;
  webserver_stop();
  lcd_force_redraw();
  ESP_LOGI(TAG, "Follow-up screen timeout => [Main screen]");
}

static void reset_trip(speedometer_t* speedo)
{
  speedometer_reset_trip(speedo);
  g_show_total = false;
  if (sdcard_reset_trip() != ESP_OK)
  {
    ESP_LOGE(TAG, "Unable to create the next trip export");
  }
}

static void format_sdcard(void)
{
  ESP_LOGI("board", "Button B (MIDDLE) => [Formatting SD]");
  if (sdcard_format() != ESP_OK)
  {
    ESP_LOGE(TAG, "Unable to format the SD card");
  }
  sdcard_get_status(&g_storage_status);
}

static void handle_button(board_button_t button, bool long_press, speedometer_t* speedo)
{
  if (!g_display_on)
  {
    turn_display_on();
    g_display_forced_off = false;
    return;
  }

  g_last_user_activity_us = esp_timer_get_time();

  if (g_wifi_menu)
  {
    if (button == BOARD_BUTTON_B && long_press)
    {
      g_wifi_menu = false;
      g_system_menu = true;
      webserver_stop();
      if (sdcard_reset_trip() != ESP_OK)
      {
        ESP_LOGE(TAG, "Unable to create the next trip export after WiFi Menu");
      }
      lcd_force_redraw();
      ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
    }
    return;
  }

  if (g_trip_info_menu)
  {
    if (button == BOARD_BUTTON_B)
    {
      g_trip_info_menu = false;
      g_list_trips_menu = true;
      lcd_force_redraw();
      ESP_LOGI("board", "Trip Info => [Closed]");
    }
    return;
  }

  if (g_list_trips_menu)
  {
    if (button == BOARD_BUTTON_B && long_press)
    {
      g_list_trips_menu = false;
      g_system_menu = true;
      lcd_force_redraw();
      ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
    }
    else if (button == BOARD_BUTTON_B && !long_press)
    {
      if (g_trip_entry_count > 0)
      {
        g_trip_info_entry = g_trip_entries[g_list_trips_selection];
        if (sdcard_get_trip_details(g_trip_base_names[g_list_trips_selection],
                                    &g_trip_info_details) != ESP_OK)
        {
          ESP_LOGE(TAG, "Unable to read trip details for %s",
                   g_trip_base_names[g_list_trips_selection]);
        }
        g_trip_info_menu = true;
        g_list_trips_menu = false;
        lcd_force_redraw();
        ESP_LOGI("board", "Trip Info => [Active]");
      }
    }
    else if (button == BOARD_BUTTON_A && !long_press)
    {
      if (g_list_trips_selection > 0)
      {
        --g_list_trips_selection;
        if (g_list_trips_selection < g_list_trips_scroll)
        {
          g_list_trips_scroll = g_list_trips_selection;
        }
      }
    }
    else if (button == BOARD_BUTTON_C && !long_press)
    {
      if (g_list_trips_selection + 1 < g_trip_entry_count)
      {
        ++g_list_trips_selection;
        if (g_list_trips_selection >= g_list_trips_scroll + LIST_TRIPS_VISIBLE_ROWS)
        {
          g_list_trips_scroll = g_list_trips_selection - LIST_TRIPS_VISIBLE_ROWS + 1;
        }
      }
    }
    return;
  }

  if (g_menu_action_active)
  {
    if (button == BOARD_BUTTON_B && !long_press)
    {
      g_menu_action_active = false;
      g_menu_action_pending = false;
      g_system_menu = true;
      lcd_force_redraw();
      ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
    }
    return;
  }

  if (g_format_confirm_menu)
  {
    if (button == BOARD_BUTTON_B && !long_press)
    {
      g_format_confirm_yes = !g_format_confirm_yes;
      lcd_force_redraw();
      ESP_LOGI("board", "Format SDcard confirm => [%s]", g_format_confirm_yes ? "Yes" : "No");
    }
    else if (button == BOARD_BUTTON_B && long_press)
    {
      g_format_confirm_menu = false;
      if (g_format_confirm_yes)
      {
        format_sdcard();
      }
      else
      {
        ESP_LOGI("board", "Format SDcard => [Cancelled]");
      }
      g_system_menu = true;
      lcd_force_redraw();
      ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
    }
    return;
  }

  if (button == BOARD_BUTTON_B && long_press)
  {
    g_system_menu = !g_system_menu;
    g_wifi_menu = false;
    g_menu_selection = 0;
    g_menu_scroll = 0;
    if (g_system_menu)
    {
      webserver_stop();
      turn_display_on();
      ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
    }
    else
    {
      webserver_stop();
      ESP_LOGI("board", "System Menu => [Closed]");
    }
    lcd_force_redraw();
    return;
  }

  if (g_system_menu)
  {
    switch (button)
    {
    case BOARD_BUTTON_A:
      if (!long_press && g_menu_selection > 0)
      {
        --g_menu_selection;
        if (g_menu_selection < g_menu_scroll)
        {
          g_menu_scroll = g_menu_selection;
        }
        log_menu_cursor(button);
      }
      break;
    case BOARD_BUTTON_B:
      if (!long_press)
      {
        if (g_menu_selection == MENU_ITEM_COUNT - 1)
        {
          g_system_menu = false;
          g_wifi_menu = false;
          webserver_stop();
          lcd_force_redraw();
          ESP_LOGI("board", "System Menu => [Closed]");
          break;
        }
        if (g_menu_selection == 2)
        {
          g_wifi_menu = true;
          g_system_menu = false;
          g_wifi_log_count = 0;
          //-- Kick off the startup sequence; the main loop advances it one
          //-- step per iteration so the screen shows up immediately and the
          //-- log lines appear as each step completes, instead of freezing.
          g_wifi_startup_state = WIFI_STARTUP_CLOSE_TRIP_FILES;
          lcd_force_redraw();
          break;
        }
        if (g_menu_selection == 1)
        {
          advance_blackout_setting();
          char label[16];
          blackout_setting_label(label, sizeof(label));
          ESP_LOGI("board", "System Menu => [Display Blackout: %s]", label);
          lcd_force_redraw();
          break;
        }
        if (g_menu_selection == 4)
        {
          refresh_trip_list();
          g_list_trips_menu = true;
          g_system_menu = false;
          lcd_force_redraw();
          ESP_LOGI("board", "List Trips => [Active]");
          break;
        }
        if (g_menu_selection == 5)
        {
          g_format_confirm_menu = true;
          g_format_confirm_yes = false;
          g_system_menu = false;
          lcd_force_redraw();
          ESP_LOGI("board", "Format SDcard => [Confirm]");
          break;
        }
        ESP_LOGI("board", "Button B (MIDDLE) => [%s]", menu_option_name(g_menu_selection));
        g_menu_action_active = true;
        g_menu_action_pending = true;
        g_menu_action_selection = g_menu_selection;
        g_menu_action_requested_us = esp_timer_get_time();
        g_system_menu = false;
        lcd_force_redraw();
      }
      break;
    case BOARD_BUTTON_C:
      if (!long_press && g_menu_selection < MENU_ITEM_COUNT - 1)
      {
        ++g_menu_selection;
        if (g_menu_selection >= g_menu_scroll + MENU_VISIBLE_ITEMS)
        {
          g_menu_scroll = g_menu_selection - MENU_VISIBLE_ITEMS + 1;
        }
        log_menu_cursor(button);
      }
      break;
    default:
      break;
    }
    return;
  }

  switch (button)
  {
  case BOARD_BUTTON_A:
    //-- Long-press trip reset removed: [System Menu] > New Trip File already
    //-- covers this and a bare long-press here was too easy to trigger by accident.
    if (!long_press)
    {
      g_trip_mode = true;
    }
    break;

  case BOARD_BUTTON_B:
    if (g_display_on)
    {
      turn_display_off(true);
    }
    else
    {
      turn_display_on();
      g_display_forced_off = false;
    }
    break;

  case BOARD_BUTTON_C:
    if (!long_press)
    {
      g_trip_mode = false;
      g_show_average = !g_show_average;
    }
    break;

  default:
    break;
  }
}

void app_main(void)
{
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  else
  {
    ESP_ERROR_CHECK(nvs_err);
  }

  ESP_ERROR_CHECK(board_init());
  ESP_ERROR_CHECK(lcd_init());

  gps_config_t gps_cfg = {
      .uart_num = UART_NUM_2,
      .rx_gpio = 16,
      .tx_gpio = 17,
      .baud_rate = 115200,
      .request_10hz = true,
  };
  ESP_ERROR_CHECK(gps_init(&gps_cfg));

  //-- Set to 1 to show the color-mapping diagnostic screen instead of the normal UI.
#define LCD_RUN_COLOR_TEST 0
#if LCD_RUN_COLOR_TEST
  lcd_set_backlight(true);
  lcd_color_test();
  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif

  esp_err_t sdcard_err = sdcard_init();
  if (sdcard_err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card unavailable: %s", esp_err_to_name(sdcard_err));
  }
  sdcard_set_status_log(sdcard_wifi_log);

  esp_err_t webserver_err = webserver_init();
  if (webserver_err != ESP_OK)
  {
    ESP_LOGE(TAG, "Webserver unavailable: %s", esp_err_to_name(webserver_err));
  }
  webserver_set_status_log(webserver_wifi_log);

  lcd_clear(LCD_COLOR_BLACK);
  lcd_set_backlight(true);

  speedometer_t speedo;
  speedometer_init(&speedo, 0.0f);

  g_last_user_activity_us = esp_timer_get_time();
  int64_t last_ui_us = 0;
  int64_t last_battery_us = 0;
  int64_t last_gpx_log_us = 0;
  gps_data_t latest_gps = {0};
  bool has_latest_gps = false;
  int battery_pct = board_battery_level();
  bool charging = board_is_charging();
  sdcard_get_status(&g_storage_status);

  ESP_LOGI(TAG, "M5Stack Speed started");
  ESP_LOGI(TAG, "GPS UART: RX=GPIO16, TX=GPIO17, 115200 8N1");

  while (true)
  {
    const int64_t now_us = esp_timer_get_time();

    board_button_event_t event;
    while (board_get_button_event(&event))
    {
      handle_button(event.button, event.long_press, &speedo);
    }

    //-- Advance the [Start Webserver] startup sequence by one step per
    //-- iteration; see wifi_startup_state_t for why this isn't done inline
    //-- in the button handler.
    switch (g_wifi_startup_state)
    {
    case WIFI_STARTUP_CLOSE_TRIP_FILES:
      if (sdcard_close_all_open_trip_files() != ESP_OK)
      {
        ESP_LOGE(TAG, "Unable to close open trip files");
      }
      g_wifi_startup_state = WIFI_STARTUP_REMOVE_SMALL;
      break;
    case WIFI_STARTUP_REMOVE_SMALL:
      if (sdcard_remove_small_trip_files() != ESP_OK)
      {
        ESP_LOGE(TAG, "Unable to remove small trip files");
      }
      g_wifi_startup_state = WIFI_STARTUP_REMOVE_UNDERSIZED;
      break;
    case WIFI_STARTUP_REMOVE_UNDERSIZED:
      if (sdcard_remove_undersized_trip_files(WIFI_MENU_MIN_TRIP_GPX_BYTES) != ESP_OK)
      {
        ESP_LOGE(TAG, "Unable to remove undersized trip files");
      }
      g_wifi_startup_state = WIFI_STARTUP_START_WEBSERVER;
      break;
    case WIFI_STARTUP_START_WEBSERVER:
      webserver_start();
      ESP_LOGI("board", "Start Webserver => [Active]");
      g_wifi_startup_state = WIFI_STARTUP_IDLE;
      break;
    case WIFI_STARTUP_IDLE:
    default:
      break;
    }

    if (!g_wifi_menu && followup_screen_is_active() &&
        now_us - g_last_user_activity_us > FOLLOWUP_SCREEN_TIMEOUT_US)
    {
      return_to_main_screen();
    }

    if (g_menu_action_active && g_menu_action_pending &&
        now_us - g_menu_action_requested_us >= 100000LL)
    {
      g_menu_action_pending = false;
      if (g_menu_action_selection == 0)
      {
        reset_trip(&speedo);
      }
      else if (g_menu_action_selection == 3)
      {
        sdcard_get_status(&g_storage_status);
      }
      else if (g_menu_action_selection == 6)
      {
        ESP_LOGI("board", "Button B (MIDDLE) => [Resetting Tracker]");
        esp_restart();
      }
    }

    if (!g_wifi_menu && webserver_get_wifi_status() != WEBSERVER_WIFI_OFF)
    {
      webserver_stop();
    }

    gps_data_t gps;
    if (gps_get_latest(&gps))
    {
      speedometer_update(&speedo, &gps, now_us);
      latest_gps = gps;
      has_latest_gps = true;
    }

    speedometer_tick(&speedo, now_us);
    speedometer_log_diagnostics(&speedo, now_us);

    if (has_latest_gps && (last_gpx_log_us == 0 || now_us - last_gpx_log_us >= 1000000LL))
    {
      last_gpx_log_us = now_us;
      if (sdcard_append_fix(&latest_gps, speedo.trip_distance_m, speedo.stationary) != ESP_OK &&
          g_storage_status.mounted)
      {
        ESP_LOGE(TAG, "Unable to append GPS fix to SD card");
      }
    }

    if ((now_us - last_battery_us) >= 2000000LL)
    {
      last_battery_us = now_us;
      battery_pct = board_battery_level();
      charging = board_is_charging();
      sdcard_get_status(&g_storage_status);
    }

    if (g_display_on && !g_display_forced_off)
    {
      uint32_t timeout_s = effective_blackout_seconds(g_blackout_minutes, battery_pct, charging);
      if (timeout_s > 0 && (now_us - g_last_user_activity_us) > (int64_t)timeout_s * 1000000LL)
      {
        turn_display_off(false);
      }
    }

    if (g_display_on && (now_us - last_ui_us) >= 50000LL)
    {
      last_ui_us = now_us;

      lcd_wifi_status_t wifi_status = LCD_WIFI_NONE;
      char wifi_ssid[33] = "";
      char wifi_ip_address[16] = "";
      if (g_system_menu || g_wifi_menu)
      {
        switch (webserver_get_wifi_status())
        {
        case WEBSERVER_WIFI_CONNECTING:
          wifi_status = LCD_WIFI_CONNECTING;
          break;
        case WEBSERVER_WIFI_STA_CONNECTED:
          wifi_status = LCD_WIFI_CONNECTED;
          break;
        case WEBSERVER_WIFI_AP_MODE:
          wifi_status = LCD_WIFI_AP_MODE;
          break;
        default:
          wifi_status = g_wifi_menu ? LCD_WIFI_CONNECTING : LCD_WIFI_NONE;
          break;
        }
        if (g_wifi_menu && wifi_status == LCD_WIFI_CONNECTED)
        {
          webserver_get_wifi_display_info(wifi_ssid, sizeof(wifi_ssid), wifi_ip_address,
                                          sizeof(wifi_ip_address));
        }
      }

      lcd_view_t view = {
          .speed_kmh = g_show_average ? speedometer_average_kmh(&speedo) : speedo.display_speed_kmh,
          .average_mode = g_show_average,
          .distance_m = g_show_total ? speedo.total_distance_m : speedo.trip_distance_m,
          .total_mode = g_show_total,
          .trip_mode = g_trip_mode,
          .gps_fix = speedo.gps_fix,
          .satellites = speedo.satellites,
          .battery_pct = battery_pct,
          .charging = charging,
          .storage_available = g_storage_status.mounted,
          .storage_free_percent = g_storage_status.free_percent,
          .storage_total_bytes = g_storage_status.total_bytes,
          .storage_free_bytes = g_storage_status.free_bytes,
          .system_menu = g_system_menu,
          .menu_selection = g_menu_selection,
          .menu_scroll = g_menu_scroll,
          .blackout_minutes = g_blackout_minutes,
          .menu_action = g_menu_action_active,
          .action_selection = g_menu_action_selection,
          .format_confirm_menu = g_format_confirm_menu,
          .format_confirm_yes = g_format_confirm_yes,
          .trip_number = sdcard_get_trip_number(),
          .point_count = sdcard_get_entry_count(),
          .wifi_status = wifi_status,
          .wifi_ssid = "",
          .wifi_ip_address = "",
          .wifi_log_count = g_wifi_log_count,
          .list_trips_menu = g_list_trips_menu,
          .trip_entries = g_trip_entries,
          .trip_entry_count = g_trip_entry_count,
          .list_trips_scroll = g_list_trips_scroll,
          .list_trips_selection = g_list_trips_selection,
          .trip_info_menu = g_trip_info_menu,
          .trip_info_entry = g_trip_info_entry,
          .trip_info_duration_s = g_trip_info_details.duration_s,
          .trip_info_avg_speed_kmh = g_trip_info_details.avg_speed_kmh,
          .trip_info_altitude_diff_m = g_trip_info_details.altitude_diff_m,
          .trip_info_end_hour = g_trip_info_details.end_hour,
          .trip_info_end_minute = g_trip_info_details.end_minute,
          .trip_info_valid = g_trip_info_details.valid,
          .prog_version = PROG_VERSION,
      };
      snprintf(view.wifi_ssid, sizeof(view.wifi_ssid), "%s", wifi_ssid);
      snprintf(view.wifi_ip_address, sizeof(view.wifi_ip_address), "%s", wifi_ip_address);
      for (uint8_t i = 0; i < g_wifi_log_count && i < LCD_WIFI_LOG_MAX_LINES; ++i)
      {
        memcpy(view.wifi_log_lines[i], g_wifi_log_lines[i], LCD_WIFI_LOG_LINE_LEN);
        view.wifi_log_colors[i] = g_wifi_log_colors[i];
      }
      sdcard_get_active_trip_datetime(view.trip_datetime, sizeof(view.trip_datetime));
      lcd_render(&view);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
