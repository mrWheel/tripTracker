#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

//-- Standard RGB565 values. The panel's Display Inversion is now disabled in
//-- lcd_init() (cmd 0x20), so these constants no longer need pre-correction.
#define LCD_COLOR_BLACK 0x0000
#define LCD_COLOR_WHITE 0xFFFF
#define LCD_COLOR_GREEN 0x07E0
#define LCD_COLOR_YELLOW 0xFFE0
#define LCD_COLOR_RED 0xF800
#define LCD_COLOR_PURPLE 0x8010
#define LCD_COLOR_CYAN 0x07FF
#define LCD_COLOR_DARKGREY 0x4208
#define LCD_COLOR_BLUE 0x001F
#define LCD_COLOR_MAGENTA 0xF81F

typedef enum
{
  LCD_WIFI_NONE,
  LCD_WIFI_CONNECTING,
  LCD_WIFI_CONNECTED,
  LCD_WIFI_AP_MODE,
} lcd_wifi_status_t;

//-- Scrolling status log shown on the [Start Webserver] screen.
#define LCD_WIFI_LOG_MAX_LINES 6
#define LCD_WIFI_LOG_LINE_LEN 40

typedef struct
{
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  float distance_m;
} lcd_trip_entry_t;

typedef struct
{
  float speed_kmh;
  bool average_mode;
  float distance_m;
  bool total_mode;
  bool trip_mode;
  bool gps_fix;
  uint8_t satellites;
  int battery_pct;
  bool charging;
  bool storage_available;
  uint8_t storage_free_percent;
  uint64_t storage_total_bytes;
  uint64_t storage_free_bytes;
  bool system_menu;
  bool storage_details;
  uint8_t menu_selection;
  uint8_t menu_scroll;
  uint8_t blackout_minutes;
  bool menu_action;
  uint8_t action_selection;
  bool format_confirm_menu;
  bool format_confirm_yes;
  uint16_t trip_number;
  uint32_t point_count;
  lcd_wifi_status_t wifi_status;
  char wifi_ssid[33];
  char wifi_ip_address[16];
  char wifi_log_lines[LCD_WIFI_LOG_MAX_LINES][LCD_WIFI_LOG_LINE_LEN];
  uint16_t wifi_log_colors[LCD_WIFI_LOG_MAX_LINES];
  uint8_t wifi_log_count;
  bool list_trips_menu;
  const lcd_trip_entry_t* trip_entries;
  size_t trip_entry_count;
  size_t list_trips_scroll;
  size_t list_trips_selection;
  bool trip_info_menu;
  lcd_trip_entry_t trip_info_entry;
  uint32_t trip_info_duration_s;
  float trip_info_avg_speed_kmh;
  float trip_info_altitude_diff_m;
  uint8_t trip_info_end_hour;
  uint8_t trip_info_end_minute;
  bool trip_info_valid;
  const char* prog_version;
  char trip_datetime[24];
} lcd_view_t;

esp_err_t lcd_init(void);
void lcd_set_backlight(bool on);
void lcd_clear(uint16_t color);
void lcd_render(const lcd_view_t* view);
void lcd_force_redraw(void);

//-- Diagnostic screen: draws labeled bars of RGB565 primary/secondary colors
//-- (unswapped values) so the on-screen photo can be used to verify color mapping.
void lcd_color_test(void);
