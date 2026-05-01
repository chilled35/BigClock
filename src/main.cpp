// =============================================================
// Big NTP Clock — Seeed Studio XIAO ESP32-C3
//
// Features:
//   - NTP time sync with auto BST/GMT changeover
//   - 7-segment LED display (252 LEDs across 4 digits)
//   - Downlight LED strip (14 LEDs)
//   - Auto-brightness via LDR light sensor
//   - Live colour control via web interface
//   - Secret effects page with rainbow animation and presets
//   - Home Assistant integration via MQTT discovery
//   - Hot tub temperature display (received via MQTT)
//   - OTA firmware updates
//
// Hardware wiring:
//   D3 (GPIO5) — Clock digit strip  (252 LEDs, NEO_GRB)
//   D2 (GPIO4) — Downlight strip    (14 LEDs,  NEO_GRB)
//   A0 (GPIO2) — Light sensor       (12-bit ADC, ADC1)
//
// Build environment:
//   VS Code + PlatformIO extension
//   Board: Seeed Studio XIAO ESP32-C3 (seeed_xiao_esp32c3)
//   Platform: espressif32
//   See platformio.ini for full config and library dependencies
// =============================================================

// --- ESP32 system headers for brownout detector control ---
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

// --- Core libraries ---
#include <Adafruit_NeoPixel.h>   // LED strip control
#include <ArduinoOTA.h>          // Over-the-air firmware updates
#include <ESPmDNS.h>             // mDNS so clock is reachable as bigclock.local
#include <NTPClient.h>           // NTP time synchronisation
#include <Preferences.h>         // Persistent flash storage for config
#define MQTT_MAX_PACKET_SIZE 1024 // Must be defined before PubSubClient include
#include <PubSubClient.h>        // MQTT client (Nick O'Leary)
#include <TimeLib.h>             // Time helper functions (hour(), minute() etc)
#include <WebServer.h>           // HTTP web server for config UI
#include <WiFi.h>                // WiFi connectivity
#include <WiFiClient.h>          // TCP client (used by MQTT)
#include <WiFiUdp.h>             // UDP (used by NTP)

// =============================================================
// HARDWARE PIN DEFINITIONS
// =============================================================
#define PIN_CLOCK       5     // D3 — data line for clock digit strip
#define PIN_DOWNLIGHT   4     // D2 — data line for downlight strip
#define PIN_LDR         2     // A0 — analogue input from light-dependent resistor
#define NUM_CLOCK       252   // 4 digits x 7 segments x 9 LEDs
#define NUM_DOWNLIGHT   14    // decorative downlight strip

// NeoPixel strip objects — NEO_GRB means the strip expects Green, Red, Blue byte order
Adafruit_NeoPixel clock_strip(NUM_CLOCK,     PIN_CLOCK,     NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel down_strip (NUM_DOWNLIGHT, PIN_DOWNLIGHT, NEO_GRB + NEO_KHZ800);

// =============================================================
// WIFI CREDENTIALS
// Change these to match your network before flashing
// =============================================================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// =============================================================
// MQTT CONFIGURATION
// The clock connects to an MQTT broker (e.g. Mosquitto running
// as a Home Assistant add-on) and uses MQTT discovery to
// automatically create entities in Home Assistant.
// =============================================================
const char* MQTT_HOST = "homeassistant.local"; // broker hostname or IP
const int   MQTT_PORT = 1883;                  // standard MQTT port
const char* MQTT_USER = "YOUR_MQTT_USERNAME";  // HA user credentials
const char* MQTT_PASS = "YOUR_MQTT_PASSWORD";
const char* MQTT_ID   = "bigclock";            // client ID shown in broker logs

// MQTT topic definitions — all under the "bigclock/" namespace
#define T_ROOT   "bigclock"
#define T_AVAIL  T_ROOT "/availability"  // online/offline status
#define T_CMD    T_ROOT "/cmd"           // inbound commands from HA
#define T_STATE  T_ROOT "/state"         // outbound state JSON (colours, BST etc)
#define T_HOTTUB T_ROOT "/hottub"        // inbound hot tub temperature

// Home Assistant MQTT discovery prefix — must match HA's discovery_prefix setting
// (default is "homeassistant")
#define HA_DISC  "homeassistant"

WiFiClient   wifi_client;        // TCP connection used by MQTT
PubSubClient mqtt(wifi_client);  // MQTT client instance

// Forward declarations so mqtt_connect() and the callback can call these
void mqtt_publish_state();
void mqtt_publish_discovery();

// =============================================================
// HOT TUB TEMPERATURE
// Received via MQTT from a Home Assistant automation that
// publishes the sensor state whenever it changes.
// -1 means no value received yet — clock just shows time.
// =============================================================
int g_hottub_temp = -1;

// =============================================================
// NTP CLIENT
// Syncs to the UK NTP pool. Offset is kept at 0 here — BST/GMT
// adjustment is applied manually in software so we always have
// the raw UTC epoch available for changeover calculations.
// =============================================================
WiFiUDP   ntp_udp;
NTPClient ntp(ntp_udp, "0.uk.pool.ntp.org", 0, 60000);

// =============================================================
// WEB SERVER
// Serves the config UI on port 80. Accessible at:
//   http://bigclock.local  or  http://<ip-address>
// =============================================================
WebServer http(80);

// =============================================================
// PERSISTENT CONFIG
// Colours and timezone settings are stored in ESP32 flash using
// the Preferences library so they survive power cycles.
// =============================================================
Preferences prefs;

// =============================================================
// COLOUR TYPES AND HELPERS
// =============================================================

// Simple RGB struct — all colour values are stored as this
struct RGB { uint8_t r, g, b; };

// Default colours on first boot (before any saved config exists)
RGB col_hour = {255,   0,   0};  // red
RGB col_min  = {  0, 255,   0};  // green
RGB col_down = {255, 255, 255};  // white

// Convert RGB struct to the 32-bit packed colour format NeoPixel expects
inline uint32_t px(RGB c) { return clock_strip.Color(c.r, c.g, c.b); }

// Parse a CSS hex colour string like "#FF2200" into an RGB struct
RGB fromHex(const char* s) {
  if (s && s[0] == '#' && strlen(s) == 7) {
    long v = strtol(s + 1, nullptr, 16);
    return { uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
  }
  return {255, 0, 0};  // fallback to red on bad input
}

// Format an RGB struct back to a CSS hex string (e.g. "#FF2200")
void toHex(RGB c, char* buf) {
  snprintf(buf, 8, "#%02X%02X%02X", c.r, c.g, c.b);
}

// Convert HSV colour (hue 0-255, saturation 0-255, value 0-255) to RGB.
// Used for rainbow animation and randomise function to generate vivid colours.
RGB hsv(uint8_t h, uint8_t s, uint8_t v) {
  if (s == 0) return {v, v, v};
  uint8_t region = h / 43;
  uint8_t rem    = (h - region * 43) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
  switch (region) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
  }
}

// =============================================================
// TIMEZONE / BST AUTO-CHANGEOVER
// The UK switches between GMT (UTC+0) and BST (UTC+1) on the
// last Sunday of March and October respectively at 01:00 UTC.
// =============================================================
bool g_auto_bst  = true;   // true = automatically track BST/GMT
bool g_is_bst    = false;  // current state — true = BST active
int  g_tz_offset = 0;      // applied to UTC epoch: 0 = GMT, 3600 = BST

// Returns the Unix timestamp of the last Sunday in a given month/year
// at the specified UTC hour. Used to calculate changeover moments.
time_t last_sunday(int year, int month, int hour_utc) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = 31;  // start beyond month end, mktime normalises it
  t.tm_hour = hour_utc;
  mktime(&t);
  t.tm_mday -= t.tm_wday;  // subtract weekday offset to reach previous Sunday
  return mktime(&t);
}

// Called once per hour from the main loop. Checks whether BST should
// be active and updates g_is_bst and g_tz_offset accordingly.
// Does nothing if manual timezone override is in effect.
void bst_check() {
  if (!g_auto_bst) return;
  time_t utc = (time_t)ntp.getEpochTime();
  struct tm tmp;
  gmtime_r(&utc, &tmp);
  int y = tmp.tm_year + 1900;
  bool should = (utc >= last_sunday(y, 3, 1) && utc < last_sunday(y, 10, 1));
  if (should != g_is_bst) {
    g_is_bst    = should;
    g_tz_offset = g_is_bst ? 3600 : 0;
  }
}

// =============================================================
// FLASH CONFIG — save and load
// Stores the current colour settings and timezone preference
// so they are restored after a power cycle or reboot.
// =============================================================
void cfg_save() {
  prefs.begin("bigclock", false);
  prefs.putUChar("hourR", col_hour.r); prefs.putUChar("hourG", col_hour.g); prefs.putUChar("hourB", col_hour.b);
  prefs.putUChar("minR",  col_min.r);  prefs.putUChar("minG",  col_min.g);  prefs.putUChar("minB",  col_min.b);
  prefs.putUChar("downR", col_down.r); prefs.putUChar("downG", col_down.g); prefs.putUChar("downB", col_down.b);
  prefs.putBool("autoBST", g_auto_bst);
  prefs.putBool("isBST",   g_is_bst);
  prefs.end();
}

void cfg_load() {
  prefs.begin("bigclock", true);  // true = read-only
  col_hour.r = prefs.getUChar("hourR", 255); col_hour.g = prefs.getUChar("hourG",   0); col_hour.b = prefs.getUChar("hourB",   0);
  col_min.r  = prefs.getUChar("minR",    0); col_min.g  = prefs.getUChar("minG",  255); col_min.b  = prefs.getUChar("minB",    0);
  col_down.r = prefs.getUChar("downR", 255); col_down.g = prefs.getUChar("downG", 255); col_down.b = prefs.getUChar("downB", 255);
  g_auto_bst = prefs.getBool("autoBST", true);
  g_is_bst   = prefs.getBool("isBST",  false);
  prefs.end();
  g_tz_offset = g_is_bst ? 3600 : 0;
}

// =============================================================
// TIME
// g_hh, g_mm, g_ss hold the current local time.
// g_last_ss is used to detect when the second has changed so
// we only redraw the display once per second.
// =============================================================
uint8_t g_hh = 0, g_mm = 0, g_ss = 0, g_last_ss = 255;

// Updates g_hh/mm/ss from the NTP epoch, applying timezone offset
void time_update() {
  time_t t = (time_t)ntp.getEpochTime() + g_tz_offset;
  g_hh = hour(t);
  g_mm = minute(t);
  g_ss = second(t);
}

// =============================================================
// LIGHT SENSOR (LDR) — AUTO BRIGHTNESS
// Reads the ambient light level from a light-dependent resistor
// and maps it to a LED brightness value. A rolling average over
// 8 samples smooths out sudden changes.
// ADC range: 0 (dark) to 4095 (bright)
// Brightness range: 255 (dark room) down to 20 (bright room)
// Note: brighter room = dimmer LEDs to avoid being blinding
// =============================================================
static const int LDR_SAMPLES = 8;
int  ldr_buf[LDR_SAMPLES];
int  ldr_idx = 0;
long ldr_sum = 2048 * LDR_SAMPLES;  // primed at midpoint

int ldr_read() {
  // Replace oldest sample with new reading
  ldr_sum -= ldr_buf[ldr_idx];
  ldr_buf[ldr_idx] = analogRead(PIN_LDR);
  ldr_sum += ldr_buf[ldr_idx];
  ldr_idx = (ldr_idx + 1) % LDR_SAMPLES;
  int avg = ldr_sum / LDR_SAMPLES;
  int bright = constrain(map(avg, 0, 4095, 255, 20), 20, 255);
  // Log to serial every 2 seconds for debugging
  static unsigned long t_ldr = 0;
  if (millis() - t_ldr >= 2000) {
    Serial.printf("LDR raw=%d  avg=%d  brightness=%d\n",
                  ldr_buf[(ldr_idx - 1 + LDR_SAMPLES) % LDR_SAMPLES], avg, bright);
    t_ldr = millis();
  }
  return bright;
}

// =============================================================
// RAINBOW ANIMATION
// Paints the clock digits with a sweeping rainbow wave that
// moves left to right. Each vertical column of pixels shares
// the same hue, and the hue offset advances each frame.
// g_hue is incremented every 10ms in the main loop.
// =============================================================
bool    g_rainbow = false;  // true = animation running
uint8_t g_hue     = 0;      // current hue offset (0-255, wraps)

// Returns true if a given pixel index within a digit's 63-pixel block
// is lit for the specified digit (0-9). Used by rainbow_frame() to
// skip pixels that would be dark in a normal display_time() call.
bool pixel_active(int digit, int base, int pixel_idx) {
  int p = pixel_idx - base;
  if (p < 0 || p >= 63) return false;
  switch (digit) {
    case 0: return (p < 27) || (p >= 36 && p < 63);
    case 1: return (p <  9) || (p >= 36 && p < 45);
    case 2: return (p < 18) || (p >= 27 && p < 36) || (p >= 45);
    case 3: return (p < 18) || (p >= 27);
    case 4: return (p <  9) || (p >= 18 && p < 45);
    case 5: return (p >=  9 && p < 54);
    case 6: return (p >=  9);
    case 7: return (p < 18) || (p >= 36 && p < 45);
    case 8: return true;
    case 9: return (p < 45);
    default: return false;
  }
}

// Renders one frame of the rainbow animation. Called every 10ms.
// Maps each active pixel to an X column position and assigns a
// hue based on that column, creating a sweeping left-to-right wave.
void rainbow_frame() {
  int digits[4] = { g_hh / 10, g_hh % 10, g_mm / 10, g_mm % 10 };
  int bases[4]  = { 189, 126, 63, 0 };
  const int TOTAL_COLS = 44;

  clock_strip.clear();

  for (int d = 0; d < 4; d++) {
    int digit_col_offset = (3 - d) * 11;
    int digit = digits[d];

    for (int p = 0; p < 63; p++) {
      if (!pixel_active(digit, bases[d], bases[d] + p)) continue;

      int local_x;
      if      (p >= 18 && p <= 26) local_x = 0;
      else if (p >= 45 && p <= 53) local_x = 0;
      else if (p >= 0  && p <= 8)  local_x = 10;
      else if (p >= 36 && p <= 44) local_x = 10;
      else if (p >= 9  && p <= 17) local_x = 9 - (p - 9);
      else if (p >= 27 && p <= 35) local_x = 9 - (p - 27);
      else if (p >= 54 && p <= 62) local_x = 9 - (p - 54);
      else continue;

      int x_pos = digit_col_offset + local_x;
      uint8_t hue = g_hue + (uint8_t)((long)x_pos * 255 / (TOTAL_COLS - 1));
      RGB c = hsv(hue, 255, 255);
      clock_strip.setPixelColor(bases[d] + p, px(c));
    }
  }
  clock_strip.show();
}

// =============================================================
// DISPLAY — segment drawing and digit rendering
//
// Each digit occupies 63 pixels arranged as 7 segments of 9 LEDs:
//
//    -----       pixels  9-17  (top horizontal)
//   |     |      pixels 18-26  (left-top vertical)
//   |     |      pixels  0-8   (right-top vertical)
//    -----       pixels 27-35  (middle horizontal)
//   |     |      pixels 45-53  (left-bottom vertical)
//   |     |      pixels 36-44  (right-bottom vertical)
//    -----       pixels 54-62  (bottom horizontal)
//
// Digit pixel bases (starting pixel of each digit in the strip):
//   Digit 0 (hours tens):    base 189
//   Digit 1 (hours units):   base 126
//   Digit 2 (minutes tens):  base  63
//   Digit 3 (minutes units): base   0
// =============================================================

// Lights a contiguous run of pixels within a digit's segment block
void seg(int base, int start, int len, uint32_t c) {
  clock_strip.fill(c, base + start, len);
}

// Renders a single character at the given pixel base offset.
// Digits 0-9 are standard numerals.
// Digit 10 = degree symbol (top square using upper 4 segments)
// Digit 11 = C (capital C using top, both left verticals, and bottom)
void draw_digit(int digit, int base, uint32_t c) {
  switch (digit) {
    case 0: seg(base,  0, 27, c); seg(base, 36, 27, c); break;
    case 1: seg(base,  0,  9, c); seg(base, 36,  9, c); break;
    case 2: seg(base,  0, 18, c); seg(base, 27,  9, c); seg(base, 45, 18, c); break;
    case 3: seg(base,  0, 18, c); seg(base, 27, 27, c); break;
    case 4: seg(base,  0,  9, c); seg(base, 18, 27, c); break;
    case 5: seg(base,  9, 45, c); break;
    case 6: seg(base,  9, 54, c); break;
    case 7: seg(base,  0, 18, c); seg(base, 36,  9, c); break;
    case 8: seg(base,  0, 63, c); break;
    case 9: seg(base,  0, 45, c); break;
    case 10: // degree symbol — small square at top: right-top + top + left-top + middle
      seg(base,  0,  9, c);
      seg(base,  9,  9, c);
      seg(base, 18,  9, c);
      seg(base, 27,  9, c);
      break;
    case 11: // C — top + left-top + left-bottom + bottom
      seg(base,  9,  9, c);
      seg(base, 18,  9, c);
      seg(base, 45,  9, c);
      seg(base, 54,  9, c);
      break;
  }
}

// Renders the current time (g_hh:g_mm) across all 4 digits.
// Hours use col_hour colour, minutes use col_min colour.
void display_time() {
  clock_strip.clear();
  draw_digit(g_hh / 10, 189, px(col_hour));
  draw_digit(g_hh % 10, 126, px(col_hour));
  draw_digit(g_mm / 10,  63, px(col_min));
  draw_digit(g_mm % 10,   0, px(col_min));
  clock_strip.show();
}

// Renders the hot tub temperature as "XX C" (with degree symbol)
// across all 4 digits. Temperature is floored to integer, clamped
// to 0-99. Uses hour colour for the number, minute colour for the
// degree symbol and C.
void display_temp() {
  clock_strip.clear();
  int t = (g_hottub_temp >= 0) ? g_hottub_temp : 0;
  t = constrain(t, 0, 99);
  draw_digit(t / 10, 189, px(col_hour));
  draw_digit(t % 10, 126, px(col_hour));
  draw_digit(10,      63, px(col_min));   // degree symbol
  draw_digit(11,       0, px(col_min));   // C
  clock_strip.show();
}

// =============================================================
// BST CHANGEOVER DATE STRINGS
// Pre-computed human-readable strings shown on the web config
// page so the user can see when the next GMT/BST switch will be.
// Updated once on boot and once per hour.
// =============================================================
char g_bst_start[32] = "calculating...";
char g_bst_end[32]   = "calculating...";

void update_changeover_strings() {
  time_t utc = (time_t)ntp.getEpochTime();
  struct tm tmp;
  gmtime_r(&utc, &tmp);
  int y = tmp.tm_year + 1900;
  time_t ts = last_sunday(y, 3,  1);
  time_t te = last_sunday(y, 10, 1);
  struct tm ss, se;
  gmtime_r(&ts, &ss);
  gmtime_r(&te, &se);
  const char* mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  snprintf(g_bst_start, sizeof(g_bst_start), "%d %s %d 01:00 UTC", ss.tm_mday, mon[ss.tm_mon], y);
  snprintf(g_bst_end,   sizeof(g_bst_end),   "%d %s %d 01:00 UTC", se.tm_mday, mon[se.tm_mon], y);
}

// =============================================================
// COLOUR PRESETS
// Shared between the web effects page and MQTT HA entities.
// Each preset has a name (for HA), emoji (for web UI), and
// hex colours for hour and minute digits.
// =============================================================
struct Preset { const char* name; const char* emoji; const char* h; const char* m; };
const Preset PRESETS[] = {
  {"Fire",     "&#x1F525;", "#FF2200", "#FF6600"},
  {"Ice",      "&#x2744;",  "#00CCFF", "#FFFFFF"},
  {"Neon",     "&#x26A1;",  "#FF00FF", "#00FF44"},
  {"Sunset",   "&#x1F305;", "#FF4400", "#FF0077"},
  {"Ocean",    "&#x1F30A;", "#0033FF", "#00CCCC"},
  {"Mono",     "&#x26AA;",  "#FFFFFF", "#AAAAAA"},
  {"Candy",    "&#x1F36C;", "#FF0088", "#FFEE00"},
  {"Midnight", "&#x1F319;", "#4400FF", "#0000CC"}
};
const int NUM_PRESETS = 8;

// Applies a preset by index — stops rainbow, sets colours, redraws, saves to flash
void apply_preset(int i) {
  if (i < 0 || i >= NUM_PRESETS) return;
  g_rainbow  = false;
  col_hour   = fromHex(PRESETS[i].h);
  col_min    = fromHex(PRESETS[i].m);
  display_time();
  cfg_save();
}

// =============================================================
// WEB INTERFACE — SHARED CSS / HTML HEADER
// Stored in flash (PROGMEM) to avoid consuming RAM.
// Uses Google Fonts (Share Tech Mono + Barlow) for styling.
// =============================================================
static const char CSS[] PROGMEM =
  "<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Big Clock</title><style>"
  "@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Barlow:wght@400;600&display=swap');"
  "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
  "body{font-family:'Barlow',sans-serif;background:#0d0d0d;color:#ccc;"
       "display:flex;flex-direction:column;align-items:center;padding:12px 16px}"
  "h1{font-family:'Share Tech Mono',monospace;font-size:.85em;letter-spacing:.3em;"
      "text-transform:uppercase;color:#444;margin-bottom:4px}"
  ".clk{font-family:'Share Tech Mono',monospace;font-size:3.5em;color:#fff;"
        "letter-spacing:.1em;line-height:1;margin:4px 0 2px;cursor:pointer;"
        "-webkit-user-select:none;user-select:none;touch-action:manipulation}"
  ".sub{font-size:.75em;color:#555;letter-spacing:.1em;margin-bottom:12px}"
  ".card{width:100%;max-width:440px;background:#161616;border:1px solid #222;"
         "border-radius:12px;padding:14px;margin-bottom:10px}"
  ".ct{font-size:.7em;letter-spacing:.2em;text-transform:uppercase;color:#e94560;"
       "margin-bottom:10px;font-weight:600}"
  ".row{display:flex;align-items:center;justify-content:space-between;"
        "padding:7px 0;border-bottom:1px solid #1e1e1e}"
  ".row:last-child{border-bottom:none}"
  ".rl{font-size:.9em;color:#888}"
  "input[type=color]{width:44px;height:32px;border:1px solid #333;border-radius:6px;"
                     "background:#0d0d0d;cursor:pointer;padding:2px}"
  ".tog{position:relative;display:inline-block;width:48px;height:26px}"
  ".tog input{opacity:0;width:0;height:0}"
  ".knob{position:absolute;inset:0;background:#2a2a2a;border-radius:26px;"
         "cursor:pointer;transition:.25s;border:1px solid #333}"
  ".knob::before{content:'';position:absolute;width:20px;height:20px;"
                 "left:2px;top:2px;background:#555;border-radius:50%;transition:.25s}"
  "input:checked+.knob{background:#1a3a2a;border-color:#2a6a4a}"
  "input:checked+.knob::before{background:#4CAF50;transform:translateX(22px)}"
  ".radios{display:flex;gap:20px;padding:8px 0}"
  ".radios label{display:flex;align-items:center;gap:7px;font-size:.9em;color:#888;cursor:pointer}"
  "input[type=radio]{accent-color:#e94560;width:15px;height:15px}"
  ".dates{margin-top:8px;padding:8px 10px;background:#0d0d0d;border-radius:8px;"
          "font-size:.78em;color:#555;line-height:1.9}"
  ".dates b{color:#777;font-weight:400}"
  ".btn{display:block;width:100%;padding:13px;background:#e94560;color:#fff;"
        "border:none;border-radius:10px;font-family:'Barlow',sans-serif;"
        "font-size:.95em;font-weight:600;cursor:pointer;transition:background .2s;"
        "margin-bottom:10px;text-align:center}"
  ".btn:active{background:#c73652}"
  ".btn2{background:#1e1e1e;border:1px solid #333;color:#ccc}"
  ".btn2:active{background:#2a2a2a}"
  ".btn2.on{border-color:#e94560;color:#e94560}"
  ".back{font-size:.8em;color:#444;text-align:center;margin-top:4px;cursor:pointer;"
         "letter-spacing:.1em;text-transform:uppercase;padding:8px}"
  "#mtz{display:none}"
  "</style></head><body>";

// =============================================================
// WEB HANDLER — GET /
// Main config page. Shows current time, colour pickers for
// hour/minute/downlight, and timezone controls.
// Colour changes are sent live via fetch() with 80ms debounce
// — no save button needed.
// Double-tapping the clock display navigates to /fx.
// =============================================================
void handle_root() {
  char hh[8], mh[8], dh[8], tb[6], tz[24], buf[200];
  time_update();
  toHex(col_hour, hh); toHex(col_min, mh); toHex(col_down, dh);
  snprintf(tb, sizeof(tb), "%02d:%02d", g_hh, g_mm);
  snprintf(tz, sizeof(tz), "%s",
    g_auto_bst ? (g_is_bst ? "Auto (BST)" : "Auto (GMT)")
               : (g_is_bst ? "BST (manual)" : "GMT (manual)"));

  http.sendHeader("Cache-Control", "no-cache");
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "text/html", "");

  http.sendContent(FPSTR(CSS));
  http.sendContent(F("<h1>Big Clock</h1>"));

  snprintf(buf, sizeof(buf),
    "<div class='clk' id='clk'>%s</div><div class='sub'>%s</div>", tb, tz);
  http.sendContent(buf);

  http.sendContent(F("<div class='card'><div class='ct'>Colours</div>"));
  snprintf(buf, sizeof(buf),
    "<div class='row'><span class='rl'>Hour digits</span>"
    "<input type='color' value='%s' oninput='lv(\"hc\",this.value)'></div>", hh);
  http.sendContent(buf);
  snprintf(buf, sizeof(buf),
    "<div class='row'><span class='rl'>Minute digits</span>"
    "<input type='color' value='%s' oninput='lv(\"mc\",this.value)'></div>", mh);
  http.sendContent(buf);
  snprintf(buf, sizeof(buf),
    "<div class='row'><span class='rl'>Downlights</span>"
    "<input type='color' value='%s' oninput='lv(\"dc\",this.value)'></div>", dh);
  http.sendContent(buf);
  http.sendContent(F("</div>"));

  http.sendContent(F("<div class='card'><div class='ct'>Timezone</div>"
    "<div class='row'><span class='rl'>Auto BST / GMT</span>"
    "<label class='tog'><input type='checkbox' id='ab' onchange='tz();tog(this)'"));
  if (g_auto_bst) http.sendContent(F(" checked"));
  http.sendContent(F("><span class='knob'></span></label></div>"
    "<div id='mtz'>"));
  http.sendContent(F("<div class='radios'>"));
  snprintf(buf, sizeof(buf),
    "<label><input type='radio' name='tz' value='gmt' onchange='tz()'%s>GMT (UTC+0)</label>"
    "<label><input type='radio' name='tz' value='bst' onchange='tz()'%s>BST (UTC+1)</label>",
    !g_is_bst ? " checked" : "",
    g_is_bst  ? " checked" : "");
  http.sendContent(buf);
  http.sendContent(F("</div>"));
  http.sendContent(F("<div class='dates'>"));
  snprintf(buf, sizeof(buf), "GMT &rarr; BST &nbsp;<b>%s</b>", g_bst_start);
  http.sendContent(buf);
  snprintf(buf, sizeof(buf), "<br>BST &rarr; GMT &nbsp;<b>%s</b>", g_bst_end);
  http.sendContent(buf);
  http.sendContent(F("</div></div></div>"));

  http.sendContent(F(
    "<script>"
    "var _t;"
    "function lv(k,v){clearTimeout(_t);_t=setTimeout(function(){"
    "fetch('/set?'+k+'='+encodeURIComponent(v))},80)}"
    "function tz(){"
    "var a=document.getElementById('ab').checked;"
    "var r=document.querySelector('input[name=tz]:checked');"
    "fetch('/set?ab='+(a?1:0)+'&tz='+(r?r.value:'gmt'));}"
    "function tog(cb){document.getElementById('mtz').style.display=cb.checked?'none':'block';}"
    "tog(document.getElementById('ab'));"
    "var _tc=0,_tt;"
    "document.getElementById('clk').addEventListener('click',function(){"
    "_tc++;clearTimeout(_tt);"
    "_tt=setTimeout(function(){_tc=0},400);"
    "if(_tc>=2)window.location='/fx';});"
    "</script></body></html>"
  ));
  http.sendContent("");
}

// =============================================================
// WEB HANDLER — GET /set
// Receives colour and timezone changes from the web UI.
// Parameters: hc (hour hex), mc (minute hex), dc (downlight hex),
//             ab (auto BST 0/1), tz (gmt/bst)
// Saves to flash and publishes updated state to MQTT.
// =============================================================
void handle_set() {
  char hc[8]={0},mc[8]={0},dc[8]={0},tz[4]={0};
  bool changed = false;

  if (http.hasArg("hc")){ strncpy(hc,http.arg("hc").c_str(),7); if(hc[0]){col_hour=fromHex(hc);changed=true;}}
  if (http.hasArg("mc")){ strncpy(mc,http.arg("mc").c_str(),7); if(mc[0]){col_min =fromHex(mc);changed=true;}}
  if (http.hasArg("dc")){ strncpy(dc,http.arg("dc").c_str(),7); if(dc[0]){col_down=fromHex(dc);changed=true;}}

  if (http.hasArg("ab")) {
    g_auto_bst = (http.arg("ab") == "1");
    if (http.hasArg("tz")) {
      strncpy(tz, http.arg("tz").c_str(), 3);
      if (!g_auto_bst) {
        g_is_bst    = (strncmp(tz, "bst", 3) == 0);
        g_tz_offset = g_is_bst ? 3600 : 0;
      }
    }
    changed = true;
  }

  if (changed) {
    g_rainbow = false;
    display_time();
    cfg_save();
    mqtt_publish_state();
  }
  http.send(200, "text/plain", "ok");
}

// =============================================================
// WEB HANDLER — GET /fx  (secret effects page)
// Accessible by double-tapping the clock display on the main page.
// Provides colour pickers, randomise, rainbow toggle, and presets.
// =============================================================
void handle_fx() {
  char hh[8], mh[8], tb[6], buf[320];
  time_update();
  toHex(col_hour, hh); toHex(col_min, mh);
  snprintf(tb, sizeof(tb), "%02d:%02d", g_hh, g_mm);

  http.sendHeader("Cache-Control", "no-cache");
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "text/html", "");

  http.sendContent(FPSTR(CSS));
  http.sendContent(F("<h1>Big Clock</h1>"));
  snprintf(buf, sizeof(buf),
    "<div class='clk'>%s</div><div class='sub'>effects</div>", tb);
  http.sendContent(buf);

  http.sendContent(F("<div class='card'><div class='ct'>Colours</div>"));
  snprintf(buf, sizeof(buf),
    "<div class='row'><span class='rl'>Hours</span>"
    "<input type='color' id='hc' value='%s' oninput='lv(\"hc\",this.value)'></div>", hh);
  http.sendContent(buf);
  snprintf(buf, sizeof(buf),
    "<div class='row'><span class='rl'>Minutes</span>"
    "<input type='color' id='mc' value='%s' oninput='lv(\"mc\",this.value)'></div>", mh);
  http.sendContent(buf);
  http.sendContent(F("</div>"));

  http.sendContent(F("<div class='card'><div class='ct'>Effects</div>"));
  http.sendContent(F("<button class='btn' onclick='rnd()'>&#x1F3B2; Randomise</button>"));
  snprintf(buf, sizeof(buf),
    "<button class='btn btn2%s' id='rbtn' onclick='toggleRb()'>"
    "&#x1F308; Rainbow wave</button>",
    g_rainbow ? " on" : "");
  http.sendContent(buf);
  http.sendContent(F("</div>"));

  http.sendContent(F("<div class='card'><div class='ct'>Presets</div>"));
  http.sendContent(F("<div style='display:flex;flex-direction:column;gap:8px;margin-top:4px'>"));

  for (int i = 0; i < NUM_PRESETS; i++) {
    snprintf(buf, sizeof(buf),
      "<div onclick='preset(\"%s\",\"%s\")' style='"
      "display:flex;flex-direction:row;align-items:center;gap:12px;"
      "padding:11px 12px;background:#0d0d0d;border:1px solid #222;"
      "border-radius:8px;cursor:pointer'>",
      PRESETS[i].h, PRESETS[i].m);
    http.sendContent(buf);
    snprintf(buf, sizeof(buf),
      "<div style='width:13px;height:13px;border-radius:50%%;background:%s;flex-shrink:0'></div>"
      "<div style='width:13px;height:13px;border-radius:50%%;background:%s;flex-shrink:0'></div>"
      "<span style='font-size:.9em;color:#888'>%s %s</span></div>",
      PRESETS[i].h, PRESETS[i].m,
      PRESETS[i].emoji, PRESETS[i].name);
    http.sendContent(buf);
  }

  http.sendContent(F("</div></div>"));
  http.sendContent(F("<div class='back' onclick='window.location=\"/\"'>&#x2190; back</div>"));

  http.sendContent(F(
    "<script>"
    "var _t;"
    "function lv(k,v){clearTimeout(_t);_t=setTimeout(function(){"
    "fetch('/set?'+k+'='+encodeURIComponent(v))},80)}"
    "function rnd(){"
    "fetch('/random').then(r=>r.json()).then(function(d){"
    "document.getElementById('hc').value=d.hc;"
    "document.getElementById('mc').value=d.mc;"
    "document.getElementById('rbtn').classList.remove('on');"
    "})}"
    "function preset(h,m){"
    "document.getElementById('hc').value=h;"
    "document.getElementById('mc').value=m;"
    "fetch('/set?hc='+encodeURIComponent(h)+'&mc='+encodeURIComponent(m));}"
    "function toggleRb(){"
    "fetch('/rainbow').then(r=>r.json()).then(function(d){"
    "var b=document.getElementById('rbtn');"
    "if(d.active){b.classList.add('on')}else{b.classList.remove('on')}})}"
    "</script></body></html>"
  ));
  http.sendContent("");
}

// =============================================================
// WEB HANDLER — GET /random
// Picks random vivid HSV colours for hour and minute digits.
// Returns JSON with the new colours and rainbow state.
// =============================================================
void handle_random() {
  g_rainbow = false;
  col_hour = hsv(random(256), 255, 220);
  col_min  = hsv(random(256), 255, 220);
  display_time();
  cfg_save();
  mqtt_publish_state();
  char hh[8], mh[8], buf[60];
  toHex(col_hour, hh); toHex(col_min, mh);
  snprintf(buf, sizeof(buf), "{\"hc\":\"%s\",\"mc\":\"%s\",\"rb\":false}", hh, mh);
  http.send(200, "application/json", buf);
}

// =============================================================
// WEB HANDLER — GET /rainbow
// Toggles the rainbow animation on or off.
// Returns JSON with the new active state.
// =============================================================
void handle_rainbow() {
  g_rainbow = !g_rainbow;
  if (!g_rainbow) display_time();
  mqtt_publish_state();
  char buf[24];
  snprintf(buf, sizeof(buf), "{\"active\":%s}", g_rainbow ? "true" : "false");
  http.send(200, "application/json", buf);
}

void handle_404() {
  http.send(404, "text/plain", "Not found");
}

// =============================================================
// MQTT — PUBLISH STATE
// Publishes the current colour settings and flags as a retained
// JSON message to bigclock/state. Home Assistant reads this to
// keep its entity states in sync with the actual clock state.
// =============================================================
void mqtt_publish_state() {
  if (!mqtt.connected()) return;
  char hh[8], mh[8], dh[8], buf[160];
  toHex(col_hour, hh); toHex(col_min, mh); toHex(col_down, dh);
  snprintf(buf, sizeof(buf),
    "{\"hour_color\":\"%s\",\"min_color\":\"%s\",\"down_color\":\"%s\","
    "\"auto_bst\":%s,\"is_bst\":%s,\"rainbow\":%s}",
    hh, mh, dh,
    g_auto_bst ? "true" : "false",
    g_is_bst   ? "true" : "false",
    g_rainbow  ? "true" : "false");
  mqtt.publish(T_STATE, buf, true);  // retained = HA gets current state on reconnect
}

// =============================================================
// MQTT — PUBLISH DISCOVERY
// Publishes MQTT discovery config messages that tell Home Assistant
// to automatically create entities for the clock — no YAML needed.
// Called once on each MQTT connect (after broker reconnects).
//
// Creates: 3 light entities (RGB colour pickers),
//          2 switches (Auto BST, Rainbow),
//          1 select (Timezone GMT/BST),
//          1 button (Randomise),
//          8 buttons (colour presets)
// =============================================================
void mqtt_publish_discovery() {
  char buf[650];

  // Hour colour light entity
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Hour Colour\",\"unique_id\":\"bigclock_hour_color\","
    "\"cmd_t\":\"" T_ROOT "/cmd/hc\",\"stat_t\":\"" T_STATE "\","
    "\"schema\":\"json\",\"color_mode\":true,\"supported_color_modes\":[\"rgb\"],"
    "\"r_tpl\":\"{{value_json.hour_color[1:3]|int(base=16)}}\","
    "\"g_tpl\":\"{{value_json.hour_color[3:5]|int(base=16)}}\","
    "\"b_tpl\":\"{{value_json.hour_color[5:7]|int(base=16)}}\","
    "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
    "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\",\"mdl\":\"XIAO ESP32-C3\"}}");
  mqtt.publish(HA_DISC "/light/bigclock/hour/config", buf, true);

  // Minute colour light entity
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Minute Colour\",\"unique_id\":\"bigclock_min_color\","
    "\"cmd_t\":\"" T_ROOT "/cmd/mc\",\"stat_t\":\"" T_STATE "\","
    "\"schema\":\"json\",\"color_mode\":true,\"supported_color_modes\":[\"rgb\"],"
    "\"r_tpl\":\"{{value_json.min_color[1:3]|int(base=16)}}\","
    "\"g_tpl\":\"{{value_json.min_color[3:5]|int(base=16)}}\","
    "\"b_tpl\":\"{{value_json.min_color[5:7]|int(base=16)}}\","
    "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
    "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\",\"mdl\":\"XIAO ESP32-C3\"}}");
  mqtt.publish(HA_DISC "/light/bigclock/minute/config", buf, true);

  // Downlight colour light entity
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Downlight Colour\",\"unique_id\":\"bigclock_down_color\","
    "\"cmd_t\":\"" T_ROOT "/cmd/dc\",\"stat_t\":\"" T_STATE "\","
    "\"schema\":\"json\",\"color_mode\":true,\"supported_color_modes\":[\"rgb\"],"
    "\"r_tpl\":\"{{value_json.down_color[1:3]|int(base=16)}}\","
    "\"g_tpl\":\"{{value_json.down_color[3:5]|int(base=16)}}\","
    "\"b_tpl\":\"{{value_json.down_color[5:7]|int(base=16)}}\","
    "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
    "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\",\"mdl\":\"XIAO ESP32-C3\"}}");
  mqtt.publish(HA_DISC "/light/bigclock/downlight/config", buf, true);

  // Auto BST switch
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Auto BST\",\"unique_id\":\"bigclock_auto_bst\","
    "\"cmd_t\":\"" T_CMD "\",\"stat_t\":\"" T_STATE "\","
    "\"val_tpl\":\"{{\\\"ON\\\" if value_json.auto_bst else \\\"OFF\\\"}}\","
    "\"pl_on\":\"{\\\"auto_bst\\\":true}\",\"pl_off\":\"{\\\"auto_bst\\\":false}\","
    "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
    "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\"}}");
  mqtt.publish(HA_DISC "/switch/bigclock/auto_bst/config", buf, true);

  // Timezone select (GMT / BST)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Timezone\",\"unique_id\":\"bigclock_timezone\","
    "\"cmd_t\":\"" T_CMD "\",\"stat_t\":\"" T_STATE "\","
    "\"val_tpl\":\"{{\\\"BST\\\" if value_json.is_bst else \\\"GMT\\\"}}\","
    "\"options\":[\"GMT\",\"BST\"],"
    "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
    "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\"}}");
  mqtt.publish(HA_DISC "/select/bigclock/timezone/config", buf, true);

  // Rainbow animation switch
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Rainbow\",\"unique_id\":\"bigclock_rainbow\","
    "\"cmd_t\":\"" T_CMD "\",\"stat_t\":\"" T_STATE "\","
    "\"val_tpl\":\"{{\\\"ON\\\" if value_json.rainbow else \\\"OFF\\\"}}\","
    "\"pl_on\":\"{\\\"rainbow\\\":true}\",\"pl_off\":\"{\\\"rainbow\\\":false}\","
    "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
    "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\"}}");
  mqtt.publish(HA_DISC "/switch/bigclock/rainbow/config", buf, true);

  // Randomise button
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Randomise\",\"unique_id\":\"bigclock_randomise\","
    "\"cmd_t\":\"" T_CMD "\","
    "\"pl_prs\":\"{\\\"random\\\":true}\","
    "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
    "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\"}}");
  mqtt.publish(HA_DISC "/button/bigclock/randomise/config", buf, true);

  // One button per colour preset
  for (int i = 0; i < NUM_PRESETS; i++) {
    char uid[32], topic[64];
    snprintf(uid,   sizeof(uid),   "bigclock_preset_%d", i);
    snprintf(topic, sizeof(topic), HA_DISC "/button/bigclock/preset_%d/config", i);
    snprintf(buf, sizeof(buf),
      "{\"name\":\"Preset: %s\",\"unique_id\":\"%s\","
      "\"cmd_t\":\"" T_CMD "\","
      "\"pl_prs\":\"{\\\"preset\\\":%d}\","
      "\"avty_t\":\"" T_AVAIL "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
      "\"device\":{\"ids\":\"bigclock\",\"name\":\"Big Clock\"}}",
      PRESETS[i].name, uid, i);
    mqtt.publish(topic, buf, true);
  }

  Serial.println("MQTT discovery published");
}

// =============================================================
// MQTT — INCOMING COMMAND HANDLER
// Handles JSON commands arriving on bigclock/cmd.
// Parsed manually (no ArduinoJson) to keep code self-contained.
//
// Supported commands (JSON key: value):
//   hc / mc / dc : "#RRGGBB"  — set hour/minute/downlight colour
//   auto_bst     : true/false  — toggle auto BST
//   tz           : "GMT"/"BST" — set timezone manually
//   rainbow      : true/false  — toggle rainbow animation
//   random       : true        — randomise hour + minute colours
//   preset       : 0-7         — apply a named colour preset
// =============================================================
void mqtt_handle_cmd(const char* payload) {
  // Extract a string value for a given key: "key":"value"
  auto strval = [](const char* json, const char* key, char* out, int outlen) -> bool {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < outlen - 1) out[i++] = *p++;
    out[i] = 0;
    return i > 0;
  };

  // Extract a boolean value for a given key: "key":true/false -> returns 1/0/-1
  auto boolval = [](const char* json, const char* key) -> int {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0)  return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
  };

  // Extract an integer value for a given key: "key":123
  auto intval = [](const char* json, const char* key) -> int {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
  };

  char val[8];
  bool changed = false;

  if (strval(payload, "hc", val, sizeof(val))) { col_hour = fromHex(val); changed = true; }
  if (strval(payload, "mc", val, sizeof(val))) { col_min  = fromHex(val); changed = true; }
  if (strval(payload, "dc", val, sizeof(val))) { col_down = fromHex(val); changed = true; }

  int ab = boolval(payload, "auto_bst");
  if (ab >= 0) { g_auto_bst = (ab == 1); changed = true; }

  char tz[4] = {0};
  if (strval(payload, "tz", tz, sizeof(tz))) {
    g_is_bst    = (strncmp(tz, "BST", 3) == 0);
    g_tz_offset = g_is_bst ? 3600 : 0;
    changed = true;
  }

  int rb = boolval(payload, "rainbow");
  if (rb >= 0) {
    g_rainbow = (rb == 1);
    if (!g_rainbow) display_time();
    changed = true;
  }

  int rnd = boolval(payload, "random");
  if (rnd == 1) {
    g_rainbow = false;
    col_hour = hsv(random(256), 255, 220);
    col_min  = hsv(random(256), 255, 220);
    changed = true;
  }

  int preset_idx = intval(payload, "preset");
  if (preset_idx >= 0 && preset_idx < NUM_PRESETS) {
    apply_preset(preset_idx);
    changed = true;
  }

  if (changed) {
    if (!g_rainbow) display_time();
    cfg_save();
    mqtt_publish_state();
  }
}

// =============================================================
// MQTT — CONNECT / RECONNECT
// Connects to the broker with a Last Will message so HA knows
// when the clock goes offline. On successful connect, subscribes
// to all command topics and republishes discovery + state.
// Called once in setup() and retried every 5s if connection drops.
// =============================================================
void mqtt_connect() {
  if (mqtt.connected()) return;
  Serial.print("MQTT connecting...");
  if (mqtt.connect(MQTT_ID, MQTT_USER, MQTT_PASS,
                   T_AVAIL, 0, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(T_AVAIL, "online", true);
    mqtt.subscribe(T_CMD);                  // general commands
    mqtt.subscribe(T_ROOT "/cmd/hc");       // hour colour (HA light format)
    mqtt.subscribe(T_ROOT "/cmd/mc");       // minute colour
    mqtt.subscribe(T_ROOT "/cmd/dc");       // downlight colour
    mqtt.subscribe(T_HOTTUB);              // hot tub temperature
    mqtt_publish_discovery();
    mqtt_publish_state();
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

// =============================================================
// OTA — OVER THE AIR UPDATES
// Configured for blank password (matches --auth= in platformio.ini).
// Clears the display and publishes offline status before flashing.
// Upload via PlatformIO: click the Upload button with OTA config
// active in platformio.ini, or run: pio run --target upload
// =============================================================
void setup_ota() {
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("bigclock");
  ArduinoOTA.setPassword("");  // blank — matches platformio.ini --auth=
  ArduinoOTA.onStart([]() {
    g_rainbow = false;
    clock_strip.clear(); clock_strip.show();
    down_strip.clear();  down_strip.show();
    mqtt.publish(T_AVAIL, "offline", true);
  });
  ArduinoOTA.onEnd([]()  { Serial.println("\nOTA done"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("OTA %u%%\r", p * 100 / t);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA error %u\n", e);
  });
  ArduinoOTA.begin();
}

// =============================================================
// SETUP
// Runs once on boot. Order matters:
//   1. Disable brownout detector (prevents resets during LED bursts)
//   2. Load saved config from flash
//   3. Connect to WiFi
//   4. Initialise LED strips
//   5. Set up OTA, NTP, mDNS, web server, MQTT
// =============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\nBig Clock (XIAO ESP32-C3) booting");

  // Disable the brownout detector — large LED draws can cause brief
  // voltage dips that would otherwise trigger a reset
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);

  cfg_load();                  // load colours and timezone from flash
  analogReadResolution(12);    // set ADC to 12-bit (0-4095) for LDR

  // Connect to WiFi (timeout after 20 seconds)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250); Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\nIP: " + WiFi.localIP().toString());
  else
    Serial.println("\nWiFi timeout");

  // Initialise LED strips after WiFi to ensure heap is stable
  clock_strip.begin(); clock_strip.clear(); clock_strip.show();
  down_strip.begin();  down_strip.clear();  down_strip.show();

  // Prime LDR buffer at midpoint so brightness starts at a sensible value
  for (int i = 0; i < LDR_SAMPLES; i++) ldr_buf[i] = 2048;
  ldr_sum = 2048 * LDR_SAMPLES;
  clock_strip.setBrightness(128);
  down_strip.setBrightness(128);

  setup_ota();

  ntp.begin();
  ntp.setTimeOffset(0);   // always use UTC — BST/GMT offset applied in software
  ntp.update();

  bst_check();
  update_changeover_strings();

  // mDNS lets you reach the clock at http://bigclock.local
  if (MDNS.begin("bigclock")) Serial.println("mDNS: bigclock.local");

  // Register web server routes
  http.on("/",        HTTP_GET, handle_root);     // main config page
  http.on("/set",     HTTP_GET, handle_set);       // colour/timezone update endpoint
  http.on("/fx",      HTTP_GET, handle_fx);        // secret effects page
  http.on("/random",  HTTP_GET, handle_random);    // randomise colours
  http.on("/rainbow", HTTP_GET, handle_rainbow);   // toggle rainbow animation
  http.onNotFound(handle_404);
  http.begin();

  // Set up MQTT client and connect
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);       // keepalive interval in seconds
  mqtt.setSocketTimeout(10);   // connection timeout in seconds

  // MQTT message callback — handles all incoming subscribed topics
  mqtt.setCallback([](char* topic, byte* payload, unsigned int len) {
    char msg[512];
    if (len >= sizeof(msg)) len = sizeof(msg) - 1;
    memcpy(msg, payload, len);
    msg[len] = 0;
    Serial.printf("MQTT [%s]: %s\n", topic, msg);

    // Hot tub temperature — floor to integer (e.g. 22.75 -> 22)
    if (strcmp(topic, T_HOTTUB) == 0) {
      g_hottub_temp = (int)(atof(msg));
      Serial.printf("Hottub temp: %d C\n", g_hottub_temp);
      return;
    }

    // Colour commands from HA light entities use a different JSON format:
    // {"state":"ON","color":{"r":255,"g":18,"b":10}}
    // Each colour channel has its own topic so we know which to update.
    if (strcmp(topic, T_ROOT "/cmd/hc") == 0 ||
        strcmp(topic, T_ROOT "/cmd/mc") == 0 ||
        strcmp(topic, T_ROOT "/cmd/dc") == 0) {
      const char* cp = strstr(msg, "\"color\"");
      if (cp) {
        int r = -1, g = -1, b = -1;
        const char* rp = strstr(cp, "\"r\":");
        const char* gp = strstr(cp, "\"g\":");
        const char* bp = strstr(cp, "\"b\":");
        if (rp) r = atoi(rp + 4);
        if (gp) g = atoi(gp + 4);
        if (bp) b = atoi(bp + 4);
        if (r >= 0 && g >= 0 && b >= 0) {
          RGB c = {(uint8_t)r, (uint8_t)g, (uint8_t)b};
          if (strcmp(topic, T_ROOT "/cmd/hc") == 0) col_hour = c;
          if (strcmp(topic, T_ROOT "/cmd/mc") == 0) col_min  = c;
          if (strcmp(topic, T_ROOT "/cmd/dc") == 0) col_down = c;
          g_rainbow = false;
          display_time();
          cfg_save();
          mqtt_publish_state();
        }
      }
      return;
    }

    // All other commands (presets, rainbow, random, BST etc)
    mqtt_handle_cmd(msg);
  });

  mqtt_connect();

  Serial.println("Ready");
}

// =============================================================
// MAIN LOOP
// Runs continuously. Handles:
//   - MQTT keepalive and reconnection
//   - OTA update polling
//   - Web server requests
//   - NTP sync (once per minute)
//   - BST check (once per hour)
//   - Display update (time or temperature cycling)
//   - Auto-brightness via LDR
//   - Downlight rendering
// =============================================================
void loop() {
  // MQTT — call loop() every iteration to process incoming messages
  // and send keepalive pings. Reconnect if connection dropped.
  if (!mqtt.connected()) {
    static unsigned long t_mqtt = 0;
    if (millis() - t_mqtt >= 5000) { mqtt_connect(); t_mqtt = millis(); }
  }
  mqtt.loop();

  ArduinoOTA.handle();    // check for OTA upload requests
  http.handleClient();    // serve pending web requests

  // NTP sync — once per minute
  static unsigned long t_ntp = 0;
  if (millis() - t_ntp >= 60000) { ntp.update(); t_ntp = millis(); }

  // BST auto-changeover check — once per hour
  static unsigned long t_bst = 0;
  if (millis() - t_bst >= 3600000UL) {
    bst_check();
    update_changeover_strings();
    t_bst = millis();
  }

  time_update();  // refresh g_hh/mm/ss from NTP epoch

  if (g_rainbow) {
    // Rainbow animation — advance hue and redraw every 10ms
    static unsigned long t_fx = 0;
    if (millis() - t_fx >= 10) {
      g_hue++;
      rainbow_frame();
      t_fx = millis();
    }
  } else {
    // Display cycling: show time for 10s, then temperature for 5s, repeat.
    // If no temperature has been received yet, just show time continuously.
    static unsigned long t_cycle = 0;
    static bool showing_temp = false;

    unsigned long now     = millis();
    unsigned long elapsed = now - t_cycle;

    if (g_hottub_temp >= 0) {
      if (!showing_temp && elapsed >= 10000) {
        // Switch from time to temperature
        showing_temp = true;
        t_cycle = now;
        display_temp();
      } else if (showing_temp && elapsed >= 5000) {
        // Switch back to time
        showing_temp = false;
        t_cycle = now;
        g_last_ss = 255;  // force immediate time redraw
      }
    }

    // Redraw time once per second (when second changes)
    if (!showing_temp && g_ss != g_last_ss) {
      g_last_ss = g_ss;
      display_time();
    }
  }

  // Update brightness from LDR and apply to both strips
  int bright = ldr_read();
  clock_strip.setBrightness(bright);
  down_strip.setBrightness(bright);

  // Render downlight strip with current downlight colour
  down_strip.fill(px(col_down), 0, NUM_DOWNLIGHT);
  down_strip.show();
}
