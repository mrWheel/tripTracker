#include "lcd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_HOST SPI3_HOST
#define LCD_MOSI GPIO_NUM_23
#define LCD_MISO GPIO_NUM_19
#define LCD_SCLK GPIO_NUM_18
#define LCD_CS GPIO_NUM_14
#define LCD_DC GPIO_NUM_27
#define LCD_RST GPIO_NUM_33
#define LCD_BL GPIO_NUM_32
#define LCD_W 320
#define LCD_H 240

static const char* TAG = "lcd";
static spi_device_handle_t s_spi;
static bool s_force_redraw = true;
static lcd_view_t s_prev;
static bool s_have_prev = false;

static void tx(bool data, const void* bytes, size_t len)
{
  if (!len)
    return;
  gpio_set_level(LCD_DC, data ? 1 : 0);
  spi_transaction_t t = {0};
  t.length = len * 8;
  t.tx_buffer = bytes;
  ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void cmd(uint8_t c)
{
  tx(false, &c, 1);
}

static void data8(uint8_t d)
{
  tx(true, &d, 1);
}

static void data(const uint8_t* d, size_t n)
{
  tx(true, d, n);
}

static void set_window(int x, int y, int w, int h)
{
  uint8_t d[4];
  cmd(0x2A);
  d[0] = (uint8_t)(x >> 8);
  d[1] = (uint8_t)x;
  d[2] = (uint8_t)((x + w - 1) >> 8);
  d[3] = (uint8_t)(x + w - 1);
  data(d, 4);
  cmd(0x2B);
  d[0] = (uint8_t)(y >> 8);
  d[1] = (uint8_t)y;
  d[2] = (uint8_t)((y + h - 1) >> 8);
  d[3] = (uint8_t)(y + h - 1);
  data(d, 4);
  cmd(0x2C);
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
  if (x < 0)
  {
    w += x;
    x = 0;
  }
  if (y < 0)
  {
    h += y;
    y = 0;
  }
  if (x + w > LCD_W)
    w = LCD_W - x;
  if (y + h > LCD_H)
    h = LCD_H - y;
  if (w <= 0 || h <= 0)
    return;

  set_window(x, y, w, h);
  uint8_t block[256];
  for (size_t i = 0; i < sizeof(block); i += 2)
  {
    block[i] = (uint8_t)(color >> 8);
    block[i + 1] = (uint8_t)color;
  }

  int pixels = w * h;
  gpio_set_level(LCD_DC, 1);
  while (pixels > 0)
  {
    int n = pixels > 128 ? 128 : pixels;
    spi_transaction_t t = {0};
    t.length = n * 16;
    t.tx_buffer = block;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    pixels -= n;
  }
}

void lcd_clear(uint16_t color)
{
  fill_rect(0, 0, LCD_W, LCD_H, color);
}

static const uint8_t* glyph(char c)
{
  // 5-bit rows, seven rows per glyph. Only characters used by this UI.
  static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
  static const uint8_t digits[10][7] = {{14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
                                        {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
                                        {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
                                        {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
                                        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};
  static const uint8_t letters[26][7] = {
      {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
      {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
      {14, 17, 16, 23, 17, 17, 14}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
      {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
      {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
      {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
      {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
      {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
      {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31}};
  //-- Small x-height shapes with true descenders, distinct from the capital letters above.
  static const uint8_t lowerLetters[26][7] = {
      {0, 0, 14, 1, 15, 17, 15},  {16, 16, 30, 17, 17, 17, 30}, {0, 0, 15, 16, 16, 16, 15},
      {1, 1, 15, 17, 17, 17, 15}, {0, 0, 14, 17, 31, 16, 15},   {6, 9, 8, 28, 8, 8, 8},
      {0, 0, 15, 17, 15, 1, 30},  {16, 16, 22, 25, 17, 17, 17}, {4, 0, 12, 4, 4, 4, 14},
      {2, 0, 6, 2, 2, 18, 12},    {16, 16, 18, 20, 24, 20, 18}, {12, 4, 4, 4, 4, 4, 14},
      {0, 0, 26, 21, 21, 21, 21}, {0, 0, 22, 25, 17, 17, 17},   {0, 0, 14, 17, 17, 17, 14},
      {0, 0, 30, 17, 17, 30, 16}, {0, 0, 15, 17, 17, 15, 1},    {0, 0, 22, 25, 16, 16, 16},
      {0, 0, 15, 16, 14, 1, 30},  {4, 14, 4, 4, 4, 5, 2},       {0, 0, 17, 17, 17, 19, 13},
      {0, 0, 17, 17, 17, 10, 4},  {0, 0, 17, 17, 21, 21, 10},   {0, 0, 17, 10, 4, 10, 17},
      {0, 0, 17, 17, 15, 1, 30},  {0, 0, 31, 2, 4, 8, 31}};
  static const uint8_t dot[7] = {0, 0, 0, 0, 0, 12, 12};
  static const uint8_t slash[7] = {1, 2, 2, 4, 8, 8, 16};
  static const uint8_t percent[7] = {17, 2, 4, 8, 16, 0, 17};
  static const uint8_t dash[7] = {0, 0, 0, 31, 0, 0, 0};
  static const uint8_t colon[7] = {0, 4, 4, 0, 4, 4, 0};
  static const uint8_t plus[7] = {0, 4, 4, 31, 4, 4, 0};

  if (c >= '0' && c <= '9')
    return digits[c - '0'];
  if (c >= 'A' && c <= 'Z')
    return letters[c - 'A'];
  if (c >= 'a' && c <= 'z')
    return lowerLetters[c - 'a'];
  if (c == '.')
    return dot;
  if (c == '/')
    return slash;
  if (c == '%')
    return percent;
  if (c == '-')
    return dash;
  if (c == ':')
    return colon;
  if (c == '+')
    return plus;
  return blank;
}

static int text_width(const char* s, int scale)
{
  return (int)strlen(s) * 6 * scale - scale;
}

static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
  const uint8_t* g = glyph(c);
  for (int row = 0; row < 7; ++row)
  {
    for (int col = 0; col < 5; ++col)
    {
      if (g[row] & (1 << (4 - col)))
      {
        fill_rect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

static void draw_text(int x, int y, const char* s, int scale, uint16_t color)
{
  while (*s)
  {
    draw_char(x, y, *s++, scale, color);
    x += 6 * scale;
  }
}

static void draw_text_centered(int y, const char* s, int scale, uint16_t color)
{
  int w = text_width(s, scale);
  draw_text((LCD_W - w) / 2, y, s, scale, color);
}

//-- Draws text truncated (dropping trailing characters) so it never runs
//-- past max_width pixels; used for the [Start Webserver] status log, whose
//-- lines come from filenames of unpredictable length.
static void draw_text_clipped(int x, int y, const char* s, int scale, uint16_t color, int max_width)
{
  char clipped[LCD_WIFI_LOG_LINE_LEN];
  snprintf(clipped, sizeof(clipped), "%s", s);
  while (clipped[0] && text_width(clipped, scale) > max_width)
  {
    clipped[strlen(clipped) - 1] = '\0';
  }
  draw_text(x, y, clipped, scale, color);
}

//-- Inserts ',' thousand separators into a non-negative integer string.
static void format_thousands(uint64_t value, char* out, size_t out_size)
{
  char digits[24];
  int digit_count = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
  int groups = (digit_count - 1) / 3;
  int out_len = digit_count + groups;
  if ((size_t)out_len >= out_size)
    out_len = (int)out_size - 1;
  out[out_len] = 0;

  int digit_index = digit_count - 1;
  int out_index = out_len - 1;
  int since_separator = 0;
  while (digit_index >= 0 && out_index >= 0)
  {
    out[out_index--] = digits[digit_index--];
    if (++since_separator == 3 && digit_index >= 0 && out_index >= 0)
    {
      out[out_index--] = ',';
      since_separator = 0;
    }
  }
}

//-- Scales a byte count to GB (2 decimals), MB, or KB, whichever fits best,
//-- with ',' thousand separators on the whole-number part.
static void format_storage_bytes(uint64_t bytes, char* out, size_t out_size)
{
  const uint64_t kKB = 1024ULL;
  const uint64_t kMB = 1024ULL * 1024ULL;
  const uint64_t kGB = 1024ULL * 1024ULL * 1024ULL;
  char whole_str[24];

  if (bytes >= kGB)
  {
    uint64_t whole = bytes / kGB;
    uint64_t hundredths = ((bytes % kGB) * 100ULL) / kGB;
    format_thousands(whole, whole_str, sizeof(whole_str));
    snprintf(out, out_size, "%s.%02llu GB", whole_str, (unsigned long long)hundredths);
  }
  else if (bytes >= kMB)
  {
    format_thousands(bytes / kMB, whole_str, sizeof(whole_str));
    snprintf(out, out_size, "%s MB", whole_str);
  }
  else
  {
    format_thousands(bytes / kKB, whole_str, sizeof(whole_str));
    snprintf(out, out_size, "%s KB", whole_str);
  }
}

//-- Shared top bar for menu/action screens: left-aligned title, right-aligned
//-- firmware version, and the dark-grey separator line beneath both.
static void draw_header(const char* title, const char* version)
{
  draw_text(7, 7, title, 2, LCD_COLOR_CYAN);
  if (version && version[0])
  {
    draw_text(LCD_W - text_width(version, 2) - 7, 7, version, 2, LCD_COLOR_WHITE);
  }
  fill_rect(0, 29, LCD_W, 1, LCD_COLOR_DARKGREY);
}

//-- Formats a trip distance as meters (no decimal, below 1000 m) or
//-- kilometers (one decimal, at or above 1000 m).
static void format_trip_distance(float distance_m, char* out, size_t out_size)
{
  if (distance_m >= 1000.0f)
  {
    snprintf(out, out_size, "%.1f KM", distance_m / 1000.0f);
  }
  else
  {
    snprintf(out, out_size, "%.0f M", distance_m);
  }
}

static const uint8_t seg_map[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

//-- Fills a horizontal seven-segment bar shaped like a flattened hexagon:
//-- the flat top/bottom run for most of its length, but both ends taper to
//-- a 45-degree point instead of a square corner, matching real LED digits.
static void fill_hbar_pointed(int x, int y, int len, int thick, uint16_t color)
{
  int half = thick / 2;
  for (int r = 0; r < thick; ++r)
  {
    int diff = r - half;
    int inset = diff < 0 ? -diff : diff;
    int width = len - 2 * inset;
    if (width > 0)
      fill_rect(x + inset, y + r, width, 1, color);
  }
}

//-- Vertical counterpart of fill_hbar_pointed(): flat left/right sides,
//-- points at the top and bottom ends.
static void fill_vbar_pointed(int x, int y, int len, int thick, uint16_t color)
{
  int half = thick / 2;
  for (int c = 0; c < thick; ++c)
  {
    int diff = c - half;
    int inset = diff < 0 ? -diff : diff;
    int height = len - 2 * inset;
    if (height > 0)
      fill_rect(x + c, y + inset, 1, height, color);
  }
}

//-- An "off" segment is not drawn at all (no outline); its bounding box is
//-- still cleared to black so it never keeps leftover "on" pixels from a
//-- previous digit, since digit redraws happen without a full-area clear.
static void draw_segment_h(int x, int y, int len, int thick, bool on, uint16_t color)
{
  if (on)
  {
    fill_hbar_pointed(x, y, len, thick, color);
    return;
  }
  fill_rect(x, y, len, thick, LCD_COLOR_BLACK);
}

static void draw_segment_v(int x, int y, int len, int thick, bool on, uint16_t color)
{
  if (on)
  {
    fill_vbar_pointed(x, y, len, thick, color);
    return;
  }
  fill_rect(x, y, thick, len, LCD_COLOR_BLACK);
}

static void draw_segment_digit(int x, int y, int digit, uint16_t color)
{
  const int w = 68, h = 116, t = 11;
  uint8_t m = (digit >= 0 && digit <= 9) ? seg_map[digit] : 0;
  // a (top), shifted 2px down, 1px wider on each side
  draw_segment_h(x + t - 1, y + 2, w - 2 * t + 2, t, m & 0x01, color);
  // b (top-right)
  draw_segment_v(x + w - t, y + t, h / 2 - t, t, m & 0x02, color);
  // c (bottom-right)
  draw_segment_v(x + w - t, y + h / 2, h / 2 - t, t, m & 0x04, color);
  // d (bottom), shifted 2px up, 1px wider on each side
  draw_segment_h(x + t - 1, y + h - t - 2, w - 2 * t + 2, t, m & 0x08, color);
  // e (bottom-left)
  draw_segment_v(x, y + h / 2, h / 2 - t, t, m & 0x10, color);
  // f (top-left)
  draw_segment_v(x, y + t, h / 2 - t, t, m & 0x20, color);
  // g (middle)
  draw_segment_h(x + t, y + h / 2 - t / 2, w - 2 * t, t, m & 0x40, color);
}

static void fill_circle(int cx, int cy, int radius, uint16_t color)
{
  for (int dy = -radius; dy <= radius; ++dy)
  {
    int dx = (int)lroundf(sqrtf((float)(radius * radius - dy * dy)));
    fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
  }
}

//-- Thin 1px ring so an "off" dot reads the same way as an "off" segment.
static void draw_circle_outline(int cx, int cy, int radius, uint16_t color)
{
  int inner = radius - 1;
  if (inner < 0)
    inner = 0;
  for (int dy = -radius; dy <= radius; ++dy)
  {
    int outerDx = (int)lroundf(sqrtf((float)(radius * radius - dy * dy)));
    int innerSq = inner * inner - dy * dy;
    if (innerSq > 0)
    {
      int innerDx = (int)lroundf(sqrtf((float)innerSq));
      fill_rect(cx - outerDx, cy + dy, outerDx - innerDx, 1, color);
      fill_rect(cx + innerDx, cy + dy, outerDx - innerDx, 1, color);
    }
    else
    {
      fill_rect(cx - outerDx, cy + dy, 2 * outerDx + 1, 1, color);
    }
  }
}

static void draw_dot(int cx, int cy, int radius, bool on, uint16_t color)
{
  if (on)
  {
    fill_circle(cx, cy, radius, color);
    return;
  }
  fill_circle(cx, cy, radius, LCD_COLOR_BLACK);
  draw_circle_outline(cx, cy, radius - 2, LCD_COLOR_DARKGREY);
}

//-- decimal_after: -1 = no decimal point, 0 = point after digit0, 1 = point after digit1.
//-- The outer digits sit `gap` pixels from the fixed middle digit (more than a
//-- bare seven-segment digit needs) and each round dot is centered in its gap.
static void draw_large_digits(int digit0, int digit1, int digit2, int decimal_after, uint16_t color)
{
  const int w = 68;
  const int t = 11;
  const int gap = 32;
  const int digit1_x = 126;
  const int digit0_x = digit1_x - w - gap;
  const int digit2_x = digit1_x + w + gap;
  const int y0 = 52;
  const int h = 116;
  const int dot_radius = 6;
  const int dot_y = y0 + h - t / 2;
  const int dot0_x = (digit0_x + w + digit1_x) / 2;
  const int dot1_x = (digit1_x + w + digit2_x) / 2;

  draw_segment_digit(digit0_x, y0, digit0, color);
  draw_segment_digit(digit1_x, y0, digit1, color);
  draw_segment_digit(digit2_x, y0, digit2, color);

  draw_dot(dot0_x, dot_y, dot_radius, decimal_after == 0, color);
  draw_dot(dot1_x, dot_y, dot_radius, decimal_after == 1, color);
}

//-- Splits a value into 3 seven-segment digits, sliding the decimal point so the
//-- displayed number keeps the highest precision the 3 digits can carry:
//-- <10 => x.xx, 10..99.9 => xx.x, >=100 => xxx (whole number, capped at 999).
static void compute_tiered_digits(float value, int* d0, int* d1, int* d2, int* decimal_after)
{
  float safe = fmaxf(0.0f, value);
  if (safe < 10.0f)
  {
    unsigned scaled = (unsigned)lroundf(safe * 100.0f);
    if (scaled > 999)
      scaled = 999;
    *d0 = scaled / 100;
    *d1 = (scaled / 10) % 10;
    *d2 = scaled % 10;
    *decimal_after = 0;
  }
  else if (safe < 100.0f)
  {
    unsigned scaled = (unsigned)lroundf(safe * 10.0f);
    if (scaled > 999)
      scaled = 999;
    *d0 = scaled / 100;
    *d1 = (scaled / 10) % 10;
    *d2 = scaled % 10;
    *decimal_after = 1;
  }
  else
  {
    unsigned whole = (unsigned)lroundf(safe);
    if (whole > 999)
      whole = 999;
    *d0 = whole / 100;
    *d1 = (whole / 10) % 10;
    *d2 = whole % 10;
    *decimal_after = -1;
  }
}

static void draw_speed(float speed, uint16_t color)
{
  int d0, d1, d2, decimal_after;
  compute_tiered_digits(speed, &d0, &d1, &d2, &decimal_after);
  draw_large_digits(d0, d1, d2, decimal_after, color);
}

//-- Below 1 km, distance is already at full meter precision so it is shown as a
//-- plain whole number (with leading zero blanking); at or above 1 km the value
//-- switches to km and reuses the same sliding-decimal tiering as draw_speed().
static void draw_trip_distance(float distance_m, uint16_t color)
{
  float safe = fmaxf(0.0f, distance_m);
  if (safe < 1000.0f)
  {
    unsigned whole = (unsigned)lroundf(safe);
    if (whole > 999)
      whole = 999;
    int d0 = whole >= 100 ? (int)(whole / 100) : -1;
    int d1 = whole >= 10 ? (int)((whole / 10) % 10) : -1;
    int d2 = (int)(whole % 10);
    draw_large_digits(d0, d1, d2, -1, color);
  }
  else
  {
    int d0, d1, d2, decimal_after;
    compute_tiered_digits(safe / 1000.0f, &d0, &d1, &d2, &decimal_after);
    draw_large_digits(d0, d1, d2, decimal_after, color);
  }
}

static void draw_distance_text_at(int x, int y, float distance_m, int scale, uint16_t color)
{
  unsigned distance = (unsigned)lroundf(fmaxf(0.0f, distance_m));
  char text[24];
  if (distance_m < 1000.0f)
  {
    snprintf(text, sizeof(text), "%u M", distance);
  }
  else if (distance_m < 100000.0f)
  {
    snprintf(text, sizeof(text), "%.2f KM", distance_m / 1000.0f);
  }
  else
  {
    snprintf(text, sizeof(text), "%.1f KM", distance_m / 1000.0f);
  }
  draw_text(x, y, text, scale, color);
}

static void draw_battery(int pct, bool charging)
{
  int x = 256, y = 6, w = 42, h = 16;
  fill_rect(x, y, w, h, LCD_COLOR_WHITE);
  fill_rect(x + 2, y + 2, w - 4, h - 4, LCD_COLOR_BLACK);
  fill_rect(x + w, y + 5, 3, 6, LCD_COLOR_WHITE);

  int p = pct;
  if (p < 0)
    p = 0;
  if (p > 100)
    p = 100;
  uint16_t c = p <= 25 ? LCD_COLOR_RED : (p <= 50 ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN);
  int fill = (w - 6) * p / 100;
  if (fill > 0)
    fill_rect(x + 3, y + 3, fill, h - 6, c);
  if (charging)
    draw_text(235, 7, "+", 1, LCD_COLOR_CYAN);
}

static void draw_static_frame(void)
{
  lcd_clear(LCD_COLOR_BLACK);
  fill_rect(0, 29, LCD_W, 1, LCD_COLOR_DARKGREY);
  fill_rect(0, 182, LCD_W, 1, LCD_COLOR_DARKGREY);
  fill_rect(0, 220, LCD_W, 1, LCD_COLOR_DARKGREY);
}

void lcd_color_test(void)
{
  //-- With Display Inversion now forced off in lcd_init(), these true
  //-- LCD_COLOR_* values should render as their intended colors.
  static const struct
  {
    const char* name;
    uint16_t raw;
  } bars[] = {
      {"RED", LCD_COLOR_RED},       {"GREEN", LCD_COLOR_GREEN}, {"BLUE", LCD_COLOR_BLUE},
      {"YELLOW", LCD_COLOR_YELLOW}, {"CYAN", LCD_COLOR_CYAN},   {"MAGENTA", LCD_COLOR_MAGENTA},
      {"WHITE", LCD_COLOR_WHITE},
  };
  const int count = sizeof(bars) / sizeof(bars[0]);
  const int bar_height = LCD_H / count;

  lcd_clear(LCD_COLOR_BLACK);
  for (int i = 0; i < count; ++i)
  {
    int y = i * bar_height;
    fill_rect(0, y, LCD_W, bar_height, bars[i].raw);
    draw_text(4, y + (bar_height - 14) / 2, bars[i].name, 2, LCD_COLOR_BLACK);
  }
}

esp_err_t lcd_init(void)
{
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << LCD_DC) | (1ULL << LCD_RST) | (1ULL << LCD_BL),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));
  gpio_set_level(LCD_BL, 0);

  spi_bus_config_t bus = {
      .mosi_io_num = LCD_MOSI,
      .miso_io_num = LCD_MISO,
      .sclk_io_num = LCD_SCLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4096,
  };
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev = {
      .clock_speed_hz = 40000000,
      .mode = 0,
      .spics_io_num = LCD_CS,
      .queue_size = 1,
  };
  ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &s_spi));

  gpio_set_level(LCD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(LCD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(120));

  cmd(0x01);
  vTaskDelay(pdMS_TO_TICKS(120));
  cmd(0x28);
  cmd(0xCF);
  {
    const uint8_t d[] = {0x00, 0xC1, 0x30};
    data(d, sizeof(d));
  }
  cmd(0xED);
  {
    const uint8_t d[] = {0x64, 0x03, 0x12, 0x81};
    data(d, sizeof(d));
  }
  cmd(0xE8);
  {
    const uint8_t d[] = {0x85, 0x00, 0x78};
    data(d, sizeof(d));
  }
  cmd(0xCB);
  {
    const uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
    data(d, sizeof(d));
  }
  cmd(0xF7);
  data8(0x20);
  cmd(0xEA);
  {
    const uint8_t d[] = {0x00, 0x00};
    data(d, sizeof(d));
  }
  cmd(0xC0);
  data8(0x23);
  cmd(0xC1);
  data8(0x10);
  cmd(0xC5);
  {
    const uint8_t d[] = {0x3E, 0x28};
    data(d, sizeof(d));
  }
  cmd(0xC7);
  data8(0x86);
  cmd(0x36);
  data8(0x08); // buttons below display, normal X direction, BGR
  cmd(0x3A);
  data8(0x55); // RGB565
  cmd(0xB1);
  {
    const uint8_t d[] = {0x00, 0x18};
    data(d, sizeof(d));
  }
  cmd(0xB6);
  {
    const uint8_t d[] = {0x08, 0x82, 0x27};
    data(d, sizeof(d));
  }
  cmd(0xF2);
  data8(0x00);
  cmd(0x26);
  data8(0x01);
  //-- 0x20 (INVOFF) had no visible effect on this panel; try 0x21 (INVON)
  //-- since some ILI9341/9342 clones have inversion semantics reversed.
  cmd(0x21);
  cmd(0x11);
  vTaskDelay(pdMS_TO_TICKS(120));
  cmd(0x29);
  vTaskDelay(pdMS_TO_TICKS(20));

  draw_static_frame();
  gpio_set_level(LCD_BL, 1);
  s_force_redraw = true;
  s_have_prev = false;
  ESP_LOGI(TAG, "ILI9342C initialized 320x240");
  return ESP_OK;
}

void lcd_set_backlight(bool on)
{
  gpio_set_level(LCD_BL, on ? 1 : 0);
}

void lcd_force_redraw(void)
{
  s_force_redraw = true;
}

void lcd_render(const lcd_view_t* v)
{
  if (!v)
    return;

  bool full = s_force_redraw || !s_have_prev;
  if (full)
  {
    draw_static_frame();
    s_force_redraw = false;
  }

  if (v->menu_action)
  {
    if (full || !s_prev.menu_action || v->action_selection != s_prev.action_selection ||
        v->storage_available != s_prev.storage_available ||
        v->storage_total_bytes != s_prev.storage_total_bytes ||
        v->storage_free_bytes != s_prev.storage_free_bytes ||
        v->trip_number != s_prev.trip_number || strcmp(v->trip_datetime, s_prev.trip_datetime) != 0)
    {
      const char* action = "Unknown";
      switch (v->action_selection)
      {
      case 0:
        action = "Reset Trip";
        break;
      case 1:
        action = "Used and Free";
        break;
      case 2:
        action = "Start Webserver";
        break;
      case 3:
        action = "Reset Tracker";
        break;
      case 4:
        action = "Format SDcard";
        break;
      case 5:
        action = "List Trip Files";
        break;
      default:
        break;
      }
      fill_rect(0, 0, LCD_W, LCD_H, LCD_COLOR_BLACK);
      if (v->action_selection == 1)
      {
        draw_header("SD CARD INFO", v->prog_version);
        if (v->storage_available)
        {
          uint64_t used_bytes = v->storage_total_bytes - v->storage_free_bytes;
          char storage[32];
          draw_text(18, 45, "Used:", 3, LCD_COLOR_RED);
          format_storage_bytes(used_bytes, storage, sizeof(storage));
          draw_text(34, 75, storage, 3, LCD_COLOR_WHITE);

          draw_text(18, 130, "Free:", 3, LCD_COLOR_GREEN);
          format_storage_bytes(v->storage_free_bytes, storage, sizeof(storage));
          draw_text(34, 160, storage, 3, LCD_COLOR_WHITE);
        }
        else
        {
          draw_text_centered(120, "Sd unavailable", 2, LCD_COLOR_RED);
        }
      }
      else
      {
        if (v->action_selection == 0)
        {
          draw_header("NEW TRIP FILE", v->prog_version);
          draw_text_centered(90, "New File", 3, LCD_COLOR_YELLOW);
          draw_text_centered(140, v->trip_datetime, 2, LCD_COLOR_WHITE);
        }
        else
        {
          draw_text(LCD_W - text_width(v->prog_version, 2) - 7, 7, v->prog_version, 2,
                    LCD_COLOR_WHITE);
          draw_text_centered(92, "Executing", 3, LCD_COLOR_CYAN);
          draw_text_centered(130, action, 3, LCD_COLOR_YELLOW);
        }
      }
    }
    s_prev = *v;
    s_have_prev = true;
    return;
  }

  if (v->wifi_status != LCD_WIFI_NONE && !v->system_menu)
  {
    bool log_changed =
        s_prev.wifi_log_count != v->wifi_log_count ||
        memcmp(s_prev.wifi_log_lines, v->wifi_log_lines, sizeof(v->wifi_log_lines)) != 0 ||
        memcmp(s_prev.wifi_log_colors, v->wifi_log_colors, sizeof(v->wifi_log_colors)) != 0;
    if (full || s_prev.wifi_status != v->wifi_status || s_prev.system_menu || log_changed)
    {
      fill_rect(0, 0, LCD_W, LCD_H, LCD_COLOR_BLACK);
      draw_header("Start Webserver", v->prog_version);
      int y = 45;
      for (uint8_t i = 0; i < v->wifi_log_count && i < LCD_WIFI_LOG_MAX_LINES; ++i)
      {
        uint16_t color = v->wifi_log_colors[i] ? v->wifi_log_colors[i] : LCD_COLOR_YELLOW;
        draw_text_clipped(7, y, v->wifi_log_lines[i], 2, color, LCD_W - 7);
        y += 25;
      }
      draw_text(7, 211, "Long MidKey: Close", 2, LCD_COLOR_WHITE);
    }
    s_prev = *v;
    s_have_prev = true;
    return;
  }

  if (v->system_menu)
  {
    if (full || !s_prev.system_menu || v->storage_total_bytes != s_prev.storage_total_bytes ||
        v->storage_free_bytes != s_prev.storage_free_bytes ||
        v->storage_available != s_prev.storage_available ||
        v->storage_details != s_prev.storage_details ||
        v->menu_selection != s_prev.menu_selection || v->menu_scroll != s_prev.menu_scroll ||
        v->wifi_status != s_prev.wifi_status)
    {
      fill_rect(0, 0, LCD_W, 240, LCD_COLOR_BLACK);
      draw_text(7, 7, "SYSTEM MENU", 2, LCD_COLOR_CYAN);
      if (v->wifi_status == LCD_WIFI_CONNECTED)
      {
        draw_text(LCD_W - text_width("Wifi", 2) - 7, 7, "Wifi", 2, LCD_COLOR_GREEN);
      }
      else if (v->wifi_status == LCD_WIFI_AP_MODE)
      {
        draw_text(LCD_W - text_width("Ap-mode", 2) - 7, 7, "Ap-mode", 2, LCD_COLOR_YELLOW);
      }
      else
      {
        draw_text(LCD_W - text_width(v->prog_version, 2) - 7, 7, v->prog_version, 2,
                  LCD_COLOR_WHITE);
      }
      fill_rect(0, 29, LCD_W, 1, LCD_COLOR_DARKGREY);

      static const char* const kMenuItems[] = {
          "New Trip File", "Show Used and Free", "Start Webserver",
          "Reset Tracker", "Format SDcard",      "List Trip Files",
          "Exit",
      };
      const int kMenuItemCount = 7;
      const int kVisibleMenuItems = 6;
      for (int i = 0; i < kVisibleMenuItems; ++i)
      {
        int item_index = v->menu_scroll + i;
        if (item_index >= kMenuItemCount)
        {
          break;
        }
        uint16_t color = item_index == v->menu_selection ? LCD_COLOR_PURPLE : LCD_COLOR_YELLOW;
        draw_text(18, 42 + i * 29, kMenuItems[item_index], 2, color);
      }
    }
    s_prev = *v;
    s_have_prev = true;
    return;
  }

  if (v->list_trips_menu)
  {
    if (full || !s_prev.list_trips_menu || v->trip_entry_count != s_prev.trip_entry_count ||
        v->list_trips_scroll != s_prev.list_trips_scroll ||
        v->list_trips_selection != s_prev.list_trips_selection)
    {
      fill_rect(0, 0, LCD_W, LCD_H, LCD_COLOR_BLACK);
      draw_header("List Trips", v->prog_version);

      const int row_height = 26;
      const int first_row_y = 38;
      const int max_visible_rows = (LCD_H - first_row_y) / row_height;

      if (v->trip_entry_count == 0)
      {
        draw_text_centered(110, "No trip files", 2, LCD_COLOR_YELLOW);
      }
      else
      {
        size_t visible = v->trip_entry_count - v->list_trips_scroll;
        if (visible > (size_t)max_visible_rows)
        {
          visible = (size_t)max_visible_rows;
        }
        for (size_t i = 0; i < visible; ++i)
        {
          size_t item_index = v->list_trips_scroll + i;
          const lcd_trip_entry_t* entry = &v->trip_entries[item_index];
          bool selected = item_index == v->list_trips_selection;
          char line[32];
          snprintf(line, sizeof(line), "%02u-%02u-%04u %02u:%02u", entry->day, entry->month,
                   entry->year, entry->hour, entry->minute);
          int y = first_row_y + (int)i * row_height;
          draw_text(7, y, line, 2, selected ? LCD_COLOR_WHITE : LCD_COLOR_YELLOW);

          char dist[16];
          format_trip_distance(entry->distance_m, dist, sizeof(dist));
          int dist_w = text_width(dist, 2);
          draw_text(LCD_W - 7 - dist_w, y, dist, 2, LCD_COLOR_WHITE);
        }
      }
    }
    s_prev = *v;
    s_have_prev = true;
    return;
  }

  if (v->trip_info_menu)
  {
    if (full || !s_prev.trip_info_menu)
    {
      fill_rect(0, 0, LCD_W, LCD_H, LCD_COLOR_BLACK);
      draw_header("Trip Info", v->prog_version);

      const int label_x = 18;
      const int value_x = 150;
      const int row_height = 30;
      int y = 45;

      char line[32];

      draw_text(label_x, y, "Date:", 2, LCD_COLOR_YELLOW);
      snprintf(line, sizeof(line), "%02u-%02u-%04u", v->trip_info_entry.day,
               v->trip_info_entry.month, v->trip_info_entry.year);
      draw_text(value_x, y, line, 2, LCD_COLOR_WHITE);
      y += row_height;

      draw_text(label_x, y, "Time:", 2, LCD_COLOR_YELLOW);
      snprintf(line, sizeof(line), "%02u:%02u - %02u:%02u", v->trip_info_entry.hour,
               v->trip_info_entry.minute, v->trip_info_end_hour, v->trip_info_end_minute);
      draw_text(value_x, y, line, 2, LCD_COLOR_WHITE);
      y += row_height;

      draw_text(label_x, y, "Distance:", 2, LCD_COLOR_YELLOW);
      format_trip_distance(v->trip_info_entry.distance_m, line, sizeof(line));
      draw_text(value_x, y, line, 2, LCD_COLOR_WHITE);
      y += row_height;

      draw_text(label_x, y, "Duration:", 2, LCD_COLOR_YELLOW);
      snprintf(line, sizeof(line), "%02u:%02u:%02u", (unsigned)(v->trip_info_duration_s / 3600),
               (unsigned)((v->trip_info_duration_s / 60) % 60),
               (unsigned)(v->trip_info_duration_s % 60));
      draw_text(value_x, y, line, 2, LCD_COLOR_WHITE);
      y += row_height;

      draw_text(label_x, y, "Avg Speed:", 2, LCD_COLOR_YELLOW);
      snprintf(line, sizeof(line), "%.1f km/h", v->trip_info_avg_speed_kmh);
      draw_text(value_x, y, line, 2, LCD_COLOR_WHITE);
      y += row_height;

      draw_text(label_x, y, "Alt Diff:", 2, LCD_COLOR_YELLOW);
      snprintf(line, sizeof(line), "%.0f M", v->trip_info_altitude_diff_m);
      draw_text(value_x, y, line, 2, LCD_COLOR_WHITE);
      y += row_height;

      if (!v->trip_info_valid)
      {
        draw_text_centered(y + 10, "Trip data unavailable", 2, LCD_COLOR_RED);
      }
    }
    s_prev = *v;
    s_have_prev = true;
    return;
  }

  if (full || v->gps_fix != s_prev.gps_fix || v->satellites != s_prev.satellites ||
      v->battery_pct != s_prev.battery_pct || v->charging != s_prev.charging)
  {
    fill_rect(0, 0, LCD_W, 28, LCD_COLOR_BLACK);
    draw_text(7, 7, v->gps_fix ? "GPS" : "NO GPS", 2, v->gps_fix ? LCD_COLOR_GREEN : LCD_COLOR_RED);
    char sat[16];
    snprintf(sat, sizeof(sat), "SAT:%02u", v->satellites);
    draw_text(101, 7, sat, 2, LCD_COLOR_WHITE);
    if (v->battery_pct >= 0)
    {
      char bat[12];
      snprintf(bat, sizeof(bat), "%d%%", v->battery_pct);
      draw_text(205, 7, bat, 2, LCD_COLOR_WHITE);
    }
    draw_battery(v->battery_pct, v->charging);
  }

  int current = (int)lroundf(v->speed_kmh);
  int previous = s_have_prev ? (int)lroundf(s_prev.speed_kmh) : -1;
  unsigned dist_current = (unsigned)lroundf(v->distance_m);
  unsigned dist_previous = s_have_prev ? (unsigned)lroundf(s_prev.distance_m) : ~0U;
  bool point_count_changed = !s_have_prev || v->point_count != s_prev.point_count;
  bool units_changed = s_have_prev && ((v->distance_m < 1000.0f) != (s_prev.distance_m < 1000.0f));

  if (full || v->trip_mode != s_prev.trip_mode || v->average_mode != s_prev.average_mode ||
      (v->trip_mode && (dist_current != dist_previous || units_changed)))
  {
    fill_rect(0, 31, LCD_W, 150, LCD_COLOR_BLACK);
    if (v->trip_mode)
    {
      draw_text_centered(32, "TRIP", 2, LCD_COLOR_YELLOW);
      draw_trip_distance(v->distance_m, LCD_COLOR_WHITE);
    }
    else
    {
      draw_text_centered(32, v->average_mode ? "AVG SPEED" : "SPEED", 2, LCD_COLOR_CYAN);
    }
  }

  if (!v->trip_mode && (full || current != previous || v->trip_mode != s_prev.trip_mode ||
                        v->average_mode != s_prev.average_mode))
  {
    draw_speed(v->speed_kmh, LCD_COLOR_WHITE);
  }

  if (!v->trip_mode &&
      (full || dist_current != dist_previous || v->total_mode != s_prev.total_mode ||
       units_changed || point_count_changed || v->trip_mode != s_prev.trip_mode))
  {
    fill_rect(0, 184, LCD_W, 36, LCD_COLOR_BLACK);
    draw_text(5, 191, "TRIP", 2, LCD_COLOR_YELLOW);
    //--aaw-draw_distance_text_at(86, 190, v->distance_m, 3, LCD_COLOR_YELLOW);
    draw_distance_text_at(86, 190, v->distance_m, 3, LCD_COLOR_WHITE);
    char point_count[12];
    snprintf(point_count, sizeof(point_count), "%u", (unsigned)v->point_count);
    draw_text(LCD_W - text_width(point_count, 2) - 6, 191, point_count, 2, LCD_COLOR_WHITE);
  }
  else if (v->trip_mode && (full || v->trip_mode != s_prev.trip_mode || current != previous ||
                            v->average_mode != s_prev.average_mode || point_count_changed))
  {
    fill_rect(0, 184, LCD_W, 36, LCD_COLOR_BLACK);
    const char* speed_label = v->average_mode ? "AVG SPEED" : "SPEED";
    int speed_x = v->average_mode ? 5 + text_width(speed_label, 2) + 10 : 86;
    draw_text(5, 191, speed_label, 2, LCD_COLOR_CYAN);
    char speed[16];
    snprintf(speed, sizeof(speed), "%u KM/H", current < 0 ? 0U : (unsigned)current);
    draw_text(speed_x, 190, speed, 3, LCD_COLOR_WHITE);
    char point_count[12];
    snprintf(point_count, sizeof(point_count), "%u", (unsigned)v->point_count);
    draw_text(LCD_W - text_width(point_count, 2) - 6, 191, point_count, 2, LCD_COLOR_WHITE);
  }

  if (full || v->storage_available != s_prev.storage_available ||
      v->storage_free_percent != s_prev.storage_free_percent)
  {
    fill_rect(0, 221, LCD_W, 19, LCD_COLOR_BLACK);
    if (v->storage_available)
    {
      uint16_t bar_width = 135;
      uint16_t free_width = (uint16_t)(bar_width * v->storage_free_percent / 100U);
      uint16_t used_width = bar_width - free_width;
      fill_rect(6, 225, bar_width + 2, 10, LCD_COLOR_DARKGREY);
      fill_rect(7, 226, used_width, 8, LCD_COLOR_RED);
      fill_rect(7 + used_width, 226, free_width, 8, LCD_COLOR_GREEN);
      char storage[24];
      snprintf(storage, sizeof(storage), "SD %u%% Free", v->storage_free_percent);
      int text_x = LCD_W - text_width(storage, 2) - 6;
      draw_text(text_x, 224, storage, 2, LCD_COLOR_WHITE);
    }
    else
    {
      draw_text(6, 224, "SD ERR", 2, LCD_COLOR_RED);
    }
  }

  s_prev = *v;
  s_have_prev = true;
}
