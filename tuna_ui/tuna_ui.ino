// =============================================================
// TUNA UI (LVGL v8) - Waveshare ESP32-S3-Touch-LCD-3.5B, 480x320 landscape
//   Loading -> Home -> Choose protocol -> Protocol detail (plate) -> Run
//   Modals: help menu, QR, about, confirm (cancel / emergency stop), result
//   Serial bridge to the Raspberry Pi (protocol in README.md)
// =============================================================

#include <lvgl.h>
#define DIRECT_RENDER_MODE
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include <Wire.h>
#include <string.h>
#include "esp_lcd_touch_axs15231b.h"

#define FW_VERSION "v1.0"

// Declared before any function: Arduino auto-generates prototypes above the first one.
typedef void (*action_fn)(void);

// Images generated from assets/ by tools/img2lvgl.py
LV_IMG_DECLARE(img_tuna_logo);
LV_IMG_DECLARE(img_cambrian_logo);
LV_IMG_DECLARE(img_icon_protocol);

// With no Pi bridge connected, Start run plays a simulated run so the UI can
// be tried standalone. With the bridge, Klipper drives the run screen.
#define DEMO_STEP_SECONDS   30
#define PI_TIMEOUT_MS       15000  // bridge sends Wi-Fi status every 5 s
// Serial is also the Pi link, so debug echo would go to the Pi. Keep 0 unless
// RPI_SERIAL is moved to a UART.
#define DEBUG_SERIAL_ECHO   0

// ----------------------------- CONFIG -----------------------------
#define RPI_SERIAL         Serial
#define SERIAL_BAUD        115200

#define GFX_BL             6
#define LCD_QSPI_CS        12
#define LCD_QSPI_CLK       5
#define LCD_QSPI_D0        1
#define LCD_QSPI_D1        2
#define LCD_QSPI_D2        3
#define LCD_QSPI_D3        4

#define I2C_SDA            8
#define I2C_SCL            7

// AXS15231B can't rotate 90/270 itself, so the panel runs at rotation 0
// and Arduino_Canvas rotates in software. 1 or 3 = landscape.
#define LCD_ROTATION       1
#define PANEL_NATIVE_W     320
#define PANEL_NATIVE_H     480

// ----------------------------- THEME ------------------------------
static const uint32_t COL_BG        = 0x000000;
static const uint32_t COL_CARD      = 0x161616;
static const uint32_t COL_CARD_HI   = 0x2A2A2A;
static const uint32_t COL_EDGE      = 0x3A3A3A;
static const uint32_t COL_LINE      = 0x262626;
static const uint32_t COL_TEXT      = 0xFFFFFF;
static const uint32_t COL_MUTED     = 0x9A9A9A;
static const uint32_t COL_ACCENT    = 0x3D63B8;
static const uint32_t COL_TIMER     = 0x3A5BA8;
static const uint32_t COL_STOP      = 0xD94343;
static const uint32_t COL_OK        = 0x22A04B;

static const uint32_t COL_SAMPLE    = 0x3B7BC8;
static const uint32_t COL_BEADS     = 0xE0661F;
static const uint32_t COL_ETHANOL   = 0x14998A;
static const uint32_t COL_ELUTION   = 0x7B5CE0;
static const uint32_t COL_WELL_EDGE = 0xBDBDBD;

static const lv_font_t *F12 = &lv_font_montserrat_12;
static const lv_font_t *F14 = &lv_font_montserrat_14;
static const lv_font_t *F16 = &lv_font_montserrat_16;
static const lv_font_t *F18 = &lv_font_montserrat_18;
static const lv_font_t *F28 = &lv_font_montserrat_28;
static const lv_font_t *F38 = &lv_font_montserrat_38;

static const int W = 480;
static const int H = 320;
static const int HDR_H = 60;

// ----------------------------- Display objects --------------------
TCA9554 TCA(0x20);
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_QSPI_CS, LCD_QSPI_CLK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_GFX *g = new Arduino_AXS15231B(bus, -1, 0, false, PANEL_NATIVE_W, PANEL_NATIVE_H);
Arduino_Canvas *gfx = new Arduino_Canvas(PANEL_NATIVE_W, PANEL_NATIVE_H, g, 0, 0, LCD_ROTATION);

uint32_t screenWidth, screenHeight, bufSize;
lv_disp_draw_buf_t draw_buf;
lv_color_t *disp_draw_buf1;
lv_color_t *disp_draw_buf2;
lv_disp_drv_t disp_drv;

// ----------------------------- App state --------------------------
#define MAX_PROTOCOLS 32
static char protoNames[MAX_PROTOCOLS][64];
static int protoCount = 0;
static char selectedName[64] = "";
// False until a real FILES|... arrives from the bridge - the placeholder
// "Protocol 001..005" list on screen before that is not launchable.
static bool haveRealFiles = false;

#define STEP_MAX 6
static const char *DEMO_STEPS[] = {"Binding", "Wash 1", "Wash 2", "Elution"};
static char stepNames[STEP_MAX][24];
static int stepCount = 0;        // 0 = no step info, show one overall bar
static int curStep = 1;          // 1-based
static int curStepPct = 50;      // fill of the current step's bar
static char runDesc[128] = "";
static uint32_t runTotalS = 0;   // 0 = unknown, show elapsed instead of countdown
static uint32_t runElapsedS = 0;
static bool running = false;
static bool demoRun = false;
static lv_timer_t *runTimer = nullptr;
static lv_timer_t *runStartTimeout = nullptr;  // "did the controller ever ack Start run?"

static uint32_t lastPiMsgMs = 0;
static bool wifiUp = false;
static char wifiSsid[40] = "";
static int wifiSignal = 0;
static char wifiIp[20] = "";

#define MAX_WIFI_NETS 20
struct WifiNet { char ssid[40]; int signal; bool secure; };
static WifiNet wifiNets[MAX_WIFI_NETS];
static int wifiNetCount = 0;
static bool wifiScanPending = false;
static char wifiConnectSsid[40] = "";
static lv_obj_t *scrWifi, *scrWifiPass;
static lv_obj_t *wifiListBox, *lblWifiCurText, *taWifiPass, *lblWifiPassTitle;
static lv_obj_t *wifiConnOverlay = nullptr;
static lv_timer_t *wifiConnTimeout = nullptr;

static char rxLine[768];
static size_t rxPos = 0;
static uint32_t lastTick = 0;

// ----------------------------- UI handles -------------------------
static lv_obj_t *scrLoading, *scrHome, *scrList, *scrDetail, *scrRun;
static lv_obj_t *listBox, *lblDetailTitle, *lblWifiIcon;
static lv_obj_t *lblRunTitle, *lblRunDesc, *lblTimerCap, *lblTimer, *lblStepCount;
static lv_obj_t *stepBars[STEP_MAX], *stepDots[STEP_MAX], *stepLabels[STEP_MAX];

// ----------------------------- Helpers ----------------------------
static void sendLine(const char *line) {
  RPI_SERIAL.print(line);
  RPI_SERIAL.print("\n");
#if DEBUG_SERIAL_ECHO
  Serial.print(">> ");
  Serial.println(line);
#endif
}

static bool pi_connected() { return lastPiMsgMs && millis() - lastPiMsgMs < PI_TIMEOUT_MS; }

// "rgb_01_solid_colors.gcode" -> "rgb_01_solid_colors"
static void display_name(char *dst, size_t n, const char *file) {
  strncpy(dst, file, n - 1);
  dst[n - 1] = 0;
  size_t len = strlen(dst);
  if (len > 6 && strcasecmp(dst + len - 6, ".gcode") == 0) dst[len - 6] = 0;
}

static lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// Plain, non-clickable, non-scrollable container with no theme styling.
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  return o;
}

static void fill(lv_obj_t *o, uint32_t bg, int radius) {
  lv_obj_set_style_bg_color(o, C(bg), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
}

static void outline(lv_obj_t *o, uint32_t col, int width) {
  lv_obj_set_style_border_color(o, C(col), 0);
  lv_obj_set_style_border_width(o, width, 0);
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = box(parent, x, y, w, h);
  fill(o, COL_CARD, 12);
  outline(o, COL_CARD_HI, 1);
  return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t col) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, C(col), 0);
  return l;
}

static lv_obj_t *hline(lv_obj_t *parent, int x, int y, int w) {
  lv_obj_t *o = box(parent, x, y, w, 1);
  fill(o, COL_LINE, 0);
  return o;
}

static void make_clickable(lv_obj_t *o, lv_event_cb_t cb, void *user) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(o, LV_OPA_60, LV_STATE_PRESSED);
  if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *txt,
                        uint32_t bg, const lv_font_t *f, lv_event_cb_t cb, void *user) {
  lv_obj_t *b = box(parent, x, y, w, h);
  fill(b, bg, 10);
  make_clickable(b, cb, user);
  lv_obj_center(label(b, txt, f, COL_TEXT));
  return b;
}

static lv_obj_t *circle_button(lv_obj_t *parent, int x, int y, const char *txt, lv_event_cb_t cb,
                               int d = 30, const lv_font_t *f = F14) {
  lv_obj_t *b = box(parent, x, y, d, d);
  fill(b, COL_CARD, LV_RADIUS_CIRCLE);
  outline(b, COL_EDGE, 1);
  if (cb) make_clickable(b, cb, nullptr);
  lv_obj_center(label(b, txt, f, COL_TEXT));
  return b;
}

static lv_obj_t *new_screen() {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_remove_style_all(s);
  fill(s, COL_BG, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

// "< Title" header with a divider; the whole left side is the back button.
static lv_obj_t *back_header(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb) {
  lv_obj_t *back = box(scr, 0, 0, 300, HDR_H);
  make_clickable(back, back_cb, nullptr);
  lv_obj_set_pos(label(back, LV_SYMBOL_LEFT, F18, COL_TEXT), 22, 20);
  lv_obj_t *t = label(back, title, F18, COL_TEXT);
  lv_obj_set_pos(t, 48, 19);
  hline(scr, 0, HDR_H, W);
  return t;
}

static void lvgl_service() {
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;
  lv_timer_handler();
}

static void pump_for(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    lvgl_service();
    delay(5);
  }
}

static void goHome() { lv_scr_load(scrHome); }

// ----------------------------- Modals ------------------------------
// Popup backdrop: blurred + darkened snapshot of the current screen.
// Blur = 4x downscale, box blur, bilinear upscale (a few tens of ms, once per popup).
static const int BLUR_SCALE = 4;
static const int BLUR_DIM = 110;  // brightness out of 256

static void box_blur(const uint8_t *src, uint8_t *dst, int w, int h, int r, bool horiz) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int s0 = 0, s1 = 0, s2 = 0, n = 0;
      for (int k = -r; k <= r; k++) {
        int xx = horiz ? x + k : x;
        int yy = horiz ? y : y + k;
        if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
        const uint8_t *p = src + (yy * w + xx) * 3;
        s0 += p[0]; s1 += p[1]; s2 += p[2]; n++;
      }
      uint8_t *d = dst + (y * w + x) * 3;
      d[0] = s0 / n; d[1] = s1 / n; d[2] = s2 / n;
    }
  }
}

// Position of full-res pixel i in the small image, as index + 8-bit weight.
static void blur_sample_pos(int i, int small_len, int16_t *i0, int16_t *i1, uint8_t *wt) {
  int f = ((2 * i + 1) * 256) / (2 * BLUR_SCALE) - 128;
  if (f < 0) f = 0;
  *i0 = f >> 8;
  *i1 = (*i0 + 1 < small_len) ? *i0 + 1 : *i0;
  *wt = f & 0xFF;
}

static void blur_darken(lv_color_t *px, int w, int h) {
  const int S = BLUR_SCALE, sw = w / S, sh = h / S;
  uint8_t *a = (uint8_t *)malloc(sw * sh * 3);
  uint8_t *b = (uint8_t *)malloc(sw * sh * 3);
  if (!a || !b) { free(a); free(b); return; }

  for (int sy = 0; sy < sh; sy++) {
    for (int sx = 0; sx < sw; sx++) {
      uint32_t r = 0, g = 0, bl = 0;
      for (int y = 0; y < S; y++) {
        const lv_color_t *row = px + (sy * S + y) * w + sx * S;
        for (int x = 0; x < S; x++) {
          uint16_t v = row[x].full;
          r += (v >> 11) & 31; g += (v >> 5) & 63; bl += v & 31;
        }
      }
      uint8_t *d = a + (sy * sw + sx) * 3;
      d[0] = r * 255 / (31 * S * S);
      d[1] = g * 255 / (63 * S * S);
      d[2] = bl * 255 / (31 * S * S);
    }
  }
  for (int pass = 0; pass < 2; pass++) {
    box_blur(a, b, sw, sh, 2, true);
    box_blur(b, a, sw, sh, 2, false);
  }

  for (int y = 0; y < h; y++) {
    int16_t y0, y1; uint8_t wy;
    blur_sample_pos(y, sh, &y0, &y1, &wy);
    for (int x = 0; x < w; x++) {
      int16_t x0, x1; uint8_t wx;
      blur_sample_pos(x, sw, &x0, &x1, &wx);
      const uint8_t *p00 = a + (y0 * sw + x0) * 3, *p01 = a + (y0 * sw + x1) * 3;
      const uint8_t *p10 = a + (y1 * sw + x0) * 3, *p11 = a + (y1 * sw + x1) * 3;
      uint8_t c[3];
      for (int k = 0; k < 3; k++) {
        int top = p00[k] * (256 - wx) + p01[k] * wx;
        int bot = p10[k] * (256 - wx) + p11[k] * wx;
        int v = (top * (256 - wy) + bot * wy) >> 16;
        c[k] = (v * BLUR_DIM) >> 8;
      }
      px[y * w + x].full = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3);
    }
  }
  free(a);
  free(b);
}

static void snapshot_free_cb(lv_event_t *e) {
  lv_snapshot_free((lv_img_dsc_t *)lv_event_get_user_data(e));
}

static lv_obj_t *overlay_create() {
  lv_obj_t *ov = box(lv_layer_top(), 0, 0, W, H);
  lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);

  lv_img_dsc_t *snap = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
  if (snap) {
    blur_darken((lv_color_t *)snap->data, snap->header.w, snap->header.h);
    lv_obj_t *img = lv_img_create(ov);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_src(img, snap);
    lv_obj_add_event_cb(img, snapshot_free_cb, LV_EVENT_DELETE, snap);
  } else {
    fill(ov, 0x000000, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_70, 0);
  }
  return ov;
}

static lv_obj_t *modal_card(lv_obj_t *ov, int x, int y, int w, int h) {
  lv_obj_t *c = card(ov, x, y, w, h);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // swallow taps so they don't reach the overlay
  return c;
}

static void close_modal_of(lv_obj_t *child) {
  lv_obj_t *o = child;
  while (lv_obj_get_parent(o) != lv_layer_top()) o = lv_obj_get_parent(o);
  lv_obj_del_async(o);
}

static void overlay_tap_close_cb(lv_event_t *e) {
  if (lv_event_get_target(e) == lv_event_get_current_target(e)) lv_obj_del_async(lv_event_get_current_target(e));
}

static void modal_close_cb(lv_event_t *e) { close_modal_of(lv_event_get_current_target(e)); }

static void modal_action_cb(lv_event_t *e) {
  action_fn fn = (action_fn)lv_event_get_user_data(e);
  close_modal_of(lv_event_get_current_target(e));
  if (fn) fn();
}

static void show_confirm(const char *title, const char *desc, action_fn on_confirm) {
  lv_obj_t *ov = overlay_create();
  int h = desc ? 196 : 164;
  lv_obj_t *c = modal_card(ov, 0, 0, 300, h);
  lv_obj_center(c);

  lv_obj_align(label(c, LV_SYMBOL_WARNING, F28, COL_STOP), LV_ALIGN_TOP_MID, 0, 16);
  lv_obj_align(label(c, title, F18, COL_TEXT), LV_ALIGN_TOP_MID, 0, 58);
  if (desc) {
    lv_obj_t *d = label(c, desc, F14, COL_MUTED);
    lv_obj_set_width(d, 260);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 88);
  }
  int by = h - 60;
  lv_obj_t *cancel = button(c, 22, by, 122, 40, "Cancel", COL_CARD, F14, modal_close_cb, nullptr);
  outline(cancel, COL_TEXT, 1);
  button(c, 154, by, 122, 40, "Confirm", COL_ACCENT, F14, modal_action_cb, (void *)on_confirm);
}

// Set only when the result modal being shown is for an E-stop - "Back to
// home" then needs to recover the machine first (see below), not just
// switch screens.
static bool resultIsEstopRecovery = false;
static void begin_estop_recover();

static void result_home_cb(lv_event_t *e) {
  close_modal_of(lv_event_get_current_target(e));
  if (resultIsEstopRecovery) {
    resultIsEstopRecovery = false;
    begin_estop_recover();
  } else {
    goHome();
  }
}

static void show_result(const char *title, bool ok, bool estop_recovery = false) {
  resultIsEstopRecovery = estop_recovery;
  lv_obj_t *ov = overlay_create();
  lv_obj_t *c = modal_card(ov, 0, 0, 300, 180);
  lv_obj_center(c);

  uint32_t col = ok ? COL_OK : COL_STOP;
  lv_obj_t *ring = box(c, 0, 0, 42, 42);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  outline(ring, col, 2);
  lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_center(label(ring, ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, F18, col));

  lv_obj_align(label(c, title, F18, COL_TEXT), LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_t *b = button(c, 0, 0, 150, 40, "Back to home", COL_ACCENT, F14, result_home_cb, nullptr);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void show_message(const char *title, const char *text) {
  lv_obj_t *ov = overlay_create();
  lv_obj_t *c = modal_card(ov, 0, 0, 320, 170);
  lv_obj_center(c);
  lv_obj_set_pos(label(c, title, F18, COL_TEXT), 20, 18);
  lv_obj_t *t = label(c, text, F14, COL_MUTED);
  lv_obj_set_width(t, 280);
  lv_obj_set_pos(t, 20, 52);
  lv_obj_t *b = button(c, 0, 0, 110, 38, "Close", COL_ACCENT, F14, modal_close_cb, nullptr);
  lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
}

// ----------------------------- Help popovers -----------------------
static void show_qr() {
  lv_obj_t *ov = overlay_create();
  lv_obj_add_event_cb(ov, overlay_tap_close_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *c = modal_card(ov, 220, 56, 212, 182);
  lv_obj_t *qr = box(c, 0, 0, 108, 108);
  fill(qr, 0xD9D9D9, 8);
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_t *t = label(c, "Scan the QR code for the\nprotocol overviews", F12, COL_TEXT);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -14);
}

static void show_about() {
  lv_obj_t *ov = overlay_create();
  lv_obj_add_event_cb(ov, overlay_tap_close_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *c = modal_card(ov, 212, 56, 222, 112);
  lv_obj_set_pos(label(c, "About", F16, COL_TEXT), 16, 12);
  lv_obj_set_pos(label(c, "Software version : " FW_VERSION, F14, COL_TEXT), 16, 46);
  hline(c, 16, 74, 188);
  lv_obj_set_pos(label(c, "Build : " __DATE__, F12, COL_MUTED), 16, 84);
}

static void show_help_menu() {
  static const char *items[] = {"User manual", "Protocol guides", "About", "Contact us"};
  static const action_fn actions[] = {nullptr, show_qr, show_about, nullptr};
  const int rowH = 33;

  lv_obj_t *ov = overlay_create();
  lv_obj_add_event_cb(ov, overlay_tap_close_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *c = modal_card(ov, 222, 56, 212, 46 + 4 * rowH);
  lv_obj_set_pos(label(c, "Help & Support", F16, COL_TEXT), 16, 12);
  for (int i = 0; i < 4; i++) {
    lv_obj_t *row = box(c, 0, 44 + i * rowH, 210, rowH);
    make_clickable(row, modal_action_cb, (void *)actions[i]);
    lv_obj_align(label(row, items[i], F14, COL_TEXT), LV_ALIGN_LEFT_MID, 16, 0);
    if (i < 3) hline(c, 16, 44 + (i + 1) * rowH, 178);
  }
}

// ----------------------------- Loading screen ----------------------
static void anim_text_opa(void *obj, int32_t v) {
  lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void createLoadingScreen() {
  scrLoading = new_screen();

  lv_obj_t *logo = lv_img_create(scrLoading);
  lv_img_set_src(logo, &img_tuna_logo);
  lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 100);

  lv_obj_t *row = box(scrLoading, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 14, 0);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 196);
  const char *letters[] = {"L", "O", "A", "D", "I", "N", "G"};
  for (int i = 0; i < 7; i++) {
    lv_obj_t *l = label(row, letters[i], F14, 0xBFBFBF);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, l);
    lv_anim_set_exec_cb(&a, anim_text_opa);
    lv_anim_set_values(&a, 40, 255);
    lv_anim_set_time(&a, 450);
    lv_anim_set_playback_time(&a, 450);
    lv_anim_set_delay(&a, i * 110);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
  }
}

// ----------------------------- Home screen --------------------------
// Defined later, in the Wi-Fi screens section; forward-declared so the
// Home screen's icon can jump straight to a live scan.
static void wifi_screen_refresh_current();
static void wifi_refresh_cb(lv_event_t *e);

static void help_btn_cb(lv_event_t *e) { show_help_menu(); }
static void choose_protocol_cb(lv_event_t *e) {
  lv_scr_load(scrList);
  // The very first CMD|LIST (at boot) can lose the race against Klipper/
  // Moonraker still starting up - re-ask every time this screen opens so
  // a stale placeholder list doesn't linger until the next reboot.
  if (pi_connected()) sendLine("CMD|LIST");
}

static void wifi_btn_cb(lv_event_t *e) {
  lv_scr_load(scrWifi);
  wifi_screen_refresh_current();
  wifi_refresh_cb(nullptr);
}

static void link_check_cb(lv_timer_t *t) {
  lv_obj_set_style_text_color(lblWifiIcon, C(pi_connected() && wifiUp ? COL_TEXT : COL_EDGE), 0);
}

static void createHomeScreen() {
  scrHome = new_screen();

  lv_obj_t *brand = lv_img_create(scrHome);
  lv_img_set_src(brand, &img_cambrian_logo);
  lv_obj_set_pos(brand, 16, 12);

  lblWifiIcon = lv_obj_get_child(circle_button(scrHome, 374, 7, LV_SYMBOL_WIFI, wifi_btn_cb, 42, F18), 0);
  lv_timer_create(link_check_cb, 1000, nullptr);
  circle_button(scrHome, 424, 7, "?", help_btn_cb, 42, F18);
  hline(scrHome, 0, 56, W);

  lv_obj_align(label(scrHome, "Start a new run", F14, COL_MUTED), LV_ALIGN_TOP_MID, 0, 86);

  lv_obj_t *c = card(scrHome, 25, 118, 430, 150);
  make_clickable(c, choose_protocol_cb, nullptr);
  lv_obj_t *tile = box(c, 0, 0, 56, 56);
  fill(tile, COL_CARD_HI, 10);
  lv_obj_align(tile, LV_ALIGN_TOP_MID, 0, 16);
  lv_obj_t *icon = lv_img_create(tile);
  lv_img_set_src(icon, &img_icon_protocol);
  lv_obj_center(icon);
  lv_obj_align(label(c, "Choose protocol", F18, COL_TEXT), LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_align(label(c, "Select a new run", F14, COL_MUTED), LV_ALIGN_TOP_MID, 0, 112);
}

// ----------------------------- Protocol list ------------------------
static void open_detail(int idx);

static void proto_row_cb(lv_event_t *e) { open_detail((int)(intptr_t)lv_event_get_user_data(e)); }
static void list_back_cb(lv_event_t *e) { goHome(); }

// Rows are tall enough for small fingers, and sized so exactly 4 fit in the
// list area at once; the rest scroll (a sliver of row 5 peeks in as a hint).
static const int PROTO_ROW_H = 60;

static void build_protocol_rows() {
  lv_obj_clean(listBox);
  if (protoCount == 0) {
    lv_obj_set_pos(label(listBox, "No protocols found", F14, COL_MUTED), 35, 12);
    return;
  }
  char name[64];
  for (int i = 0; i < protoCount; i++) {
    lv_obj_t *row = box(listBox, 0, i * PROTO_ROW_H, W, PROTO_ROW_H);
    make_clickable(row, proto_row_cb, (void *)(intptr_t)i);
    display_name(name, sizeof(name), protoNames[i]);
    lv_obj_align(label(row, name, F16, COL_TEXT), LV_ALIGN_LEFT_MID, 35, 0);
    lv_obj_align(label(row, LV_SYMBOL_RIGHT, F14, COL_TEXT), LV_ALIGN_RIGHT_MID, -24, 0);
    hline(row, 0, PROTO_ROW_H - 1, W);
  }
}

static void createListScreen() {
  scrList = new_screen();
  back_header(scrList, "Choose protocol", list_back_cb);
  listBox = box(scrList, 0, HDR_H + 1, W, H - HDR_H - 1);
  lv_obj_add_flag(listBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(listBox, LV_DIR_VER);
  build_protocol_rows();
}

// ----------------------------- Wi-Fi screens -------------------------
// Forward declarations: used as callbacks/by each other before their own
// definition further down (Arduino's auto-prototyping is unreliable for
// this pattern elsewhere in the file, so we're explicit, as above).
static void begin_wifi_connect(const char *ssid, const char *password);
static void wifi_net_row_cb(lv_event_t *e);

static void pct_decode(char *s);  // defined with the serial parsing, below

// Mirrors pct_decode(): percent-encodes text we send TO the Pi (SSID,
// password) so a literal | ; or space in either can't break the line format.
static void pct_encode(const char *src, char *dst, size_t dstsz) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < dstsz; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
      dst[o++] = (char)*p;
    } else {
      snprintf(dst + o, dstsz - o, "%%%02X", *p);
      o += 3;
    }
  }
  dst[o] = 0;
}

static void wifi_screen_refresh_current() {
  char buf[100];
  if (!pi_connected()) {
    strcpy(buf, "Controller not connected");
  } else if (wifiUp) {
    snprintf(buf, sizeof(buf), "Connected: %s\n%s   %d%%", wifiSsid, wifiIp, wifiSignal);
  } else {
    strcpy(buf, "Not connected to a network");
  }
  lv_label_set_text(lblWifiCurText, buf);
}

static void wifi_rows_show_message(const char *msg) {
  lv_obj_clean(wifiListBox);
  lv_obj_set_pos(label(wifiListBox, msg, F14, COL_MUTED), 20, 14);
}

static void rebuild_wifi_rows() {
  lv_obj_clean(wifiListBox);
  if (wifiNetCount == 0) {
    wifi_rows_show_message("No networks found.");
    return;
  }
  const int ROW_H = 56;
  char sub[32];
  for (int i = 0; i < wifiNetCount; i++) {
    lv_obj_t *row = box(wifiListBox, 0, i * ROW_H, W, ROW_H);
    make_clickable(row, wifi_net_row_cb, (void *)(intptr_t)i);
    lv_obj_align(label(row, wifiNets[i].ssid, F16, COL_TEXT), LV_ALIGN_TOP_LEFT, 20, 8);
    snprintf(sub, sizeof(sub), "%s   %d%%", wifiNets[i].secure ? "Secured" : "Open", wifiNets[i].signal);
    lv_obj_align(label(row, sub, F12, COL_MUTED), LV_ALIGN_BOTTOM_LEFT, 20, -8);
    lv_obj_align(label(row, LV_SYMBOL_RIGHT, F14, COL_TEXT), LV_ALIGN_RIGHT_MID, -20, 0);
    hline(row, 0, ROW_H - 1, W);
  }
}

static void wifi_scan_timeout_cb(lv_timer_t *t) {
  if (wifiScanPending) {
    wifiScanPending = false;
    wifi_rows_show_message("No response from the controller.");
  }
}

static void wifi_refresh_cb(lv_event_t *e) {
  if (!pi_connected()) {
    wifi_rows_show_message("Controller not connected.\nCheck the USB cable to the Raspberry Pi.");
    return;
  }
  wifiScanPending = true;
  wifi_rows_show_message("Scanning...");
  sendLine("CMD|WIFI_SCAN");
  lv_timer_t *t = lv_timer_create(wifi_scan_timeout_cb, 6000, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

static void wifi_list_back_cb(lv_event_t *e) { goHome(); }

static void createWifiScreen() {
  scrWifi = new_screen();
  back_header(scrWifi, "Wi-Fi", wifi_list_back_cb);
  circle_button(scrWifi, 430, 14, LV_SYMBOL_REFRESH, wifi_refresh_cb);

  lv_obj_t *cur = card(scrWifi, 20, 72, 440, 46);
  lblWifiCurText = label(cur, "", F14, COL_TEXT);
  lv_obj_set_style_text_line_space(lblWifiCurText, 2, 0);
  lv_obj_align(lblWifiCurText, LV_ALIGN_LEFT_MID, 4, 0);

  lv_obj_align(label(scrWifi, "Available networks", F14, COL_MUTED), LV_ALIGN_TOP_LEFT, 22, 128);

  wifiListBox = box(scrWifi, 0, 152, W, H - 152);
  lv_obj_add_flag(wifiListBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(wifiListBox, LV_DIR_VER);
}

// --- Password entry (secured networks) --------------------------------
static void toggle_pw_visibility_cb(lv_event_t *e) {
  bool was_hidden = lv_textarea_get_password_mode(taWifiPass);
  lv_textarea_set_password_mode(taWifiPass, !was_hidden);
  lv_obj_t *icon_lbl = lv_obj_get_child(lv_event_get_current_target(e), 0);
  lv_label_set_text(icon_lbl, was_hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

static void wifi_pass_back_cb(lv_event_t *e) { lv_scr_load(scrWifi); }

// Shared by the keyboard's own Enter key and the top "Connect" button.
static void wifi_pass_submit() {
  begin_wifi_connect(wifiConnectSsid, lv_textarea_get_text(taWifiPass));
  lv_scr_load(scrWifi);
}

static void wifi_pass_connect_btn_cb(lv_event_t *e) { wifi_pass_submit(); }

static void wifi_pass_kb_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    wifi_pass_submit();
  } else if (code == LV_EVENT_CANCEL) {
    lv_scr_load(scrWifi);
  }
}

static void createWifiPasswordScreen() {
  scrWifiPass = new_screen();

  // One compact row: back + Wi-Fi icon + SSID + Connect, all together -
  // this is the same total height (34px) as the old two-line header, so
  // the keyboard below keeps the exact 252px it needs for all 5 rows.
  lv_obj_t *top = box(scrWifiPass, 0, 0, W, 34);
  make_clickable(top, wifi_pass_back_cb, nullptr);
  lv_obj_set_pos(label(top, LV_SYMBOL_LEFT, F14, COL_TEXT), 10, 10);
  lv_obj_set_pos(label(top, LV_SYMBOL_WIFI, F14, COL_TEXT), 34, 10);
  lblWifiPassTitle = label(top, "Network", F14, COL_TEXT);
  lv_obj_set_pos(lblWifiPassTitle, 58, 9);
  button(top, 356, 3, 108, 28, "Connect", COL_ACCENT, F14, wifi_pass_connect_btn_cb, nullptr);
  hline(scrWifiPass, 0, 34, W);

  taWifiPass = lv_textarea_create(scrWifiPass);
  lv_textarea_set_one_line(taWifiPass, true);
  lv_textarea_set_password_mode(taWifiPass, true);
  lv_textarea_set_placeholder_text(taWifiPass, "Password");
  lv_obj_set_pos(taWifiPass, 12, 36);
  lv_obj_set_size(taWifiPass, 336, 32);

  circle_button(scrWifiPass, 356, 36, LV_SYMBOL_EYE_CLOSE, toggle_pw_visibility_cb, 32, F14);

  // Full-width, nearly the whole rest of the screen. The default theme's
  // per-key minimum height (meant for bigger displays) is taller than a
  // 5-row keyboard can fit here, so rows beyond what fits at that minimum
  // were simply not drawn - min_height=0 lets keys shrink to fit instead.
  // KB_Y=68 is proven to show all 5 rows fully - do not shrink this further
  // without actually re-testing on the board.
  const int KB_Y = 68;
  lv_obj_t *kb = lv_keyboard_create(scrWifiPass);
  lv_keyboard_set_textarea(kb, taWifiPass);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_style_min_height(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_min_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_pad_all(kb, 2, 0);
  lv_obj_set_style_pad_row(kb, 2, 0);
  lv_obj_set_style_pad_column(kb, 2, 0);
  lv_obj_set_style_text_font(kb, F12, LV_PART_ITEMS);
  lv_obj_set_pos(kb, 0, KB_Y);
  lv_obj_set_size(kb, W, H - KB_Y);

  // Dark-theme the keys (default is the light theme's white key look, which
  // barely showed against our black screens) and give pressed/held keys a
  // clearly different color so taps have visible feedback.
  lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, C(COL_CARD_HI), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, C(COL_TEXT), LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(kb, C(COL_EDGE), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, C(COL_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(kb, C(COL_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);

  // LVGL's keyboard sends READY/CANCEL to the bound textarea, not to the
  // keyboard widget itself - that's why Enter did nothing before.
  lv_obj_add_event_cb(taWifiPass, wifi_pass_kb_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(taWifiPass, wifi_pass_kb_cb, LV_EVENT_CANCEL, nullptr);
}

static void wifi_net_row_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= wifiNetCount) return;
  strncpy(wifiConnectSsid, wifiNets[idx].ssid, sizeof(wifiConnectSsid) - 1);
  wifiConnectSsid[sizeof(wifiConnectSsid) - 1] = 0;
  if (wifiNets[idx].secure) {
    lv_label_set_text(lblWifiPassTitle, wifiConnectSsid);
    lv_textarea_set_text(taWifiPass, "");
    lv_scr_load(scrWifiPass);
  } else {
    begin_wifi_connect(wifiConnectSsid, "");
  }
}

// --- Connecting ---------------------------------------------------------
static void wifi_connect_timeout_cb(lv_timer_t *t) {
  wifiConnTimeout = nullptr;
  if (wifiConnOverlay) {
    lv_obj_del(wifiConnOverlay);
    wifiConnOverlay = nullptr;
    show_message("Wi-Fi", "No response from the controller.");
  }
}

static void begin_wifi_connect(const char *ssid, const char *password) {
  char encSsid[100], encPw[200], cmd[320];
  pct_encode(ssid, encSsid, sizeof(encSsid));
  pct_encode(password, encPw, sizeof(encPw));
  snprintf(cmd, sizeof(cmd), "CMD|WIFI_CONNECT|%s|%s", encSsid, encPw);
  sendLine(cmd);
  strncpy(wifiConnectSsid, ssid, sizeof(wifiConnectSsid) - 1);
  wifiConnectSsid[sizeof(wifiConnectSsid) - 1] = 0;

  if (wifiConnOverlay) lv_obj_del(wifiConnOverlay);
  wifiConnOverlay = overlay_create();
  lv_obj_t *c = modal_card(wifiConnOverlay, 0, 0, 260, 150);
  lv_obj_center(c);
  lv_obj_t *sp = lv_spinner_create(c, 1000, 60);
  lv_obj_set_size(sp, 40, 40);
  lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_set_style_arc_color(sp, C(COL_ACCENT), LV_PART_INDICATOR);
  char msg[80];
  snprintf(msg, sizeof(msg), "Connecting to\n%s", ssid);
  lv_obj_t *l = label(c, msg, F14, COL_TEXT);
  lv_obj_set_width(l, 220);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -20);

  if (wifiConnTimeout) lv_timer_del(wifiConnTimeout);
  wifiConnTimeout = lv_timer_create(wifi_connect_timeout_cb, 30000, nullptr);
  lv_timer_set_repeat_count(wifiConnTimeout, 1);
}

// Called from the serial parser below on OK|WIFI... or ERR|WIFI|...
static void on_wifi_result(bool ok, const char *detail) {
  if (wifiConnTimeout) { lv_timer_del(wifiConnTimeout); wifiConnTimeout = nullptr; }
  if (wifiConnOverlay) { lv_obj_del(wifiConnOverlay); wifiConnOverlay = nullptr; }
  char msg[110];
  if (ok) snprintf(msg, sizeof(msg), "Connected to %s%s%s", wifiConnectSsid, (detail && detail[0]) ? "\n" : "", detail ? detail : "");
  else snprintf(msg, sizeof(msg), "%s", (detail && detail[0]) ? detail : "Connection failed.");
  show_message(ok ? "Wi-Fi connected" : "Wi-Fi", msg);
  wifi_screen_refresh_current();
}

// --- E-stop recovery: FIRMWARE_RESTART then G28, run on the Pi side -----
static lv_obj_t *recoverOverlay = nullptr;
static lv_timer_t *recoverTimeout = nullptr;

static void recover_timeout_cb(lv_timer_t *t) {
  recoverTimeout = nullptr;
  if (recoverOverlay) {
    lv_obj_del(recoverOverlay);
    recoverOverlay = nullptr;
    show_message("Restart", "No response from the controller. Home the machine manually before the next run.");
  }
  goHome();
}

static void begin_estop_recover() {
  sendLine("CMD|RECOVER");

  if (recoverOverlay) lv_obj_del(recoverOverlay);
  recoverOverlay = overlay_create();
  lv_obj_t *c = modal_card(recoverOverlay, 0, 0, 260, 150);
  lv_obj_center(c);
  lv_obj_t *sp = lv_spinner_create(c, 1000, 60);
  lv_obj_set_size(sp, 40, 40);
  lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_set_style_arc_color(sp, C(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_t *l = label(c, "Restarting firmware\nand homing...", F14, COL_TEXT);
  lv_obj_set_width(l, 220);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -20);

  if (recoverTimeout) lv_timer_del(recoverTimeout);
  recoverTimeout = lv_timer_create(recover_timeout_cb, 30000, nullptr);
  lv_timer_set_repeat_count(recoverTimeout, 1);
}

// Called from the serial parser below on OK|RECOVER or ERR|RECOVER|...
static void on_recover_result(bool ok, const char *detail) {
  if (recoverTimeout) { lv_timer_del(recoverTimeout); recoverTimeout = nullptr; }
  if (recoverOverlay) { lv_obj_del(recoverOverlay); recoverOverlay = nullptr; }
  if (!ok) show_message("Restart", (detail && detail[0]) ? detail : "Restart/home failed. Home the machine manually before the next run.");
  goHome();
}

// WIFI_LIST|ssid,signal,secure;ssid2,signal2,secure2;...
static void on_wifi_list(char *payload) {
  wifiScanPending = false;
  wifiNetCount = 0;
  for (char *ent = strtok(payload, ";"); ent && wifiNetCount < MAX_WIFI_NETS; ent = strtok(nullptr, ";")) {
    char *parts[3];
    int pn = 0;
    parts[pn++] = ent;
    for (char *p = ent; *p && pn < 3; p++) {
      if (*p == ',') { *p = 0; parts[pn++] = p + 1; }
    }
    if (pn < 3) continue;
    pct_decode(parts[0]);
    WifiNet &n = wifiNets[wifiNetCount++];
    strncpy(n.ssid, parts[0], sizeof(n.ssid) - 1);
    n.ssid[sizeof(n.ssid) - 1] = 0;
    n.signal = atoi(parts[1]);
    n.secure = atoi(parts[2]) != 0;
  }
  rebuild_wifi_rows();
}

// ----------------------------- Protocol detail ----------------------
// 96-well plate drawn directly (one object instead of 96).
// Demo layout; will come from protocol data later.
static void plate_draw_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);

  lv_draw_rect_dsc_t well;
  lv_draw_rect_dsc_init(&well);
  well.radius = LV_RADIUS_CIRCLE;

  lv_draw_rect_dsc_t edge;
  lv_draw_rect_dsc_init(&edge);
  edge.radius = LV_RADIUS_CIRCLE;
  edge.bg_opa = LV_OPA_TRANSP;
  edge.border_width = 1;
  edge.border_color = C(COL_WELL_EDGE);

  lv_draw_arc_dsc_t half;
  lv_draw_arc_dsc_init(&half);
  half.width = 5;
  half.color = C(COL_SAMPLE);

  for (int r = 0; r < 8; r++) {
    for (int col = 0; col < 12; col++) {
      int cx = a.x1 + 3 + col * 13 + 5;
      int cy = a.y1 + 2 + r * 13 + 5;
      lv_area_t w;
      w.x1 = cx - 5; w.y1 = cy - 5; w.x2 = cx + 5; w.y2 = cy + 5;

      uint32_t fc = 0;
      switch (col) {
        case 0: fc = COL_BEADS; break;
        case 1: case 2: fc = COL_ETHANOL; break;
        case 3: fc = COL_ELUTION; break;
      }
      if (fc) {
        well.bg_color = C(fc);
        lv_draw_rect(ctx, &well, &w);
      }
      if (col == 0) {
        lv_point_t p;
        p.x = cx; p.y = cy;
        lv_draw_arc(ctx, &half, &p, 5, 90, 270);
      }
      lv_draw_rect(ctx, &edge, &w);
    }
  }
}

static void detail_back_cb(lv_event_t *e) { lv_scr_load(scrList); }
static void start_run_cb(lv_event_t *e);

static void createDetailScreen() {
  scrDetail = new_screen();
  lblDetailTitle = back_header(scrDetail, "Protocol", detail_back_cb);

  lv_obj_set_pos(label(scrDetail, "Load & Verify Reagents", F16, COL_TEXT), 25, 72);

  lv_obj_t *c = card(scrDetail, 25, 100, 430, 136);
  lv_obj_t *plate = box(c, 20, 14, 160, 106);
  outline(plate, 0xCFCFCF, 1);
  lv_obj_set_style_radius(plate, 4, 0);
  lv_obj_add_event_cb(plate, plate_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

  static const char *names[] = {"Sample", "Beads", "85% ethanol", "Elution buffer"};
  static const uint32_t cols[] = {COL_SAMPLE, COL_BEADS, COL_ETHANOL, COL_ELUTION};
  for (int i = 0; i < 4; i++) {
    int y = 20 + i * 26;
    lv_obj_t *dot = box(c, 210, y, 12, 12);
    fill(dot, cols[i], LV_RADIUS_CIRCLE);
    lv_obj_set_pos(label(c, names[i], F14, COL_TEXT), 232, y - 2);
  }

  button(scrDetail, 25, 250, 430, 46, "Start run", COL_ACCENT, F16, start_run_cb, nullptr);
}

static void open_detail(int idx) {
  if (idx < 0 || idx >= protoCount) return;
  strncpy(selectedName, protoNames[idx], sizeof(selectedName) - 1);
  selectedName[sizeof(selectedName) - 1] = 0;
  char name[64];
  display_name(name, sizeof(name), selectedName);
  lv_label_set_text(lblDetailTitle, name);
  lv_scr_load(scrDetail);
}

// ----------------------------- Run screen ---------------------------
// Step area spans x 243..456; bars share it evenly (1..STEP_MAX bars).
static const int STEPS_X = 243, STEPS_W = 213, STEPS_GAP = 4;

static void layout_steps() {
  int n = stepCount > 0 ? stepCount : 1;
  int bw = (STEPS_W - (n - 1) * STEPS_GAP) / n;
  for (int i = 0; i < STEP_MAX; i++) {
    bool show = i < n;
    lv_obj_t *objs[] = {stepBars[i], stepDots[i], stepLabels[i]};
    for (lv_obj_t *o : objs) {
      if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!show) continue;
    int x = STEPS_X + i * (bw + STEPS_GAP);
    lv_obj_set_pos(stepBars[i], x, 146);
    lv_obj_set_width(stepBars[i], bw);
    lv_obj_set_pos(stepDots[i], x + bw / 2 - 2, 160);
    lv_label_set_text(stepLabels[i], stepCount > 0 ? stepNames[i] : "");
    lv_obj_align_to(stepLabels[i], stepBars[i], LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
  }
  if (stepCount == 0) lv_obj_add_flag(stepDots[0], LV_OBJ_FLAG_HIDDEN);
}

static void update_run_ui() {
  bool countdown = runTotalS > 0;
  uint32_t t = countdown ? (runElapsedS < runTotalS ? runTotalS - runElapsedS : 0) : runElapsedS;
  lv_label_set_text(lblTimerCap, countdown ? "COMPLETING IN" : "ELAPSED");
  if (t >= 3600)
    lv_label_set_text_fmt(lblTimer, "%lu:%02lu:%02lu", (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60), (unsigned long)(t % 60));
  else
    lv_label_set_text_fmt(lblTimer, "%02lu:%02lu", (unsigned long)(t / 60), (unsigned long)(t % 60));

  if (stepCount == 0) {
    lv_label_set_text(lblStepCount, "");
    lv_bar_set_value(stepBars[0], countdown ? runElapsedS * 100 / runTotalS : 0, LV_ANIM_OFF);
    return;
  }
  int step = curStep < 1 ? 1 : (curStep > stepCount ? stepCount : curStep);
  lv_label_set_text_fmt(lblStepCount, "%d of %d", step, stepCount);
  for (int i = 0; i < stepCount; i++) {
    int v = i < step - 1 ? 100 : (i == step - 1 ? curStepPct : 0);
    lv_bar_set_value(stepBars[i], v, LV_ANIM_OFF);
    lv_obj_set_style_text_color(stepLabels[i], C(i == step - 1 ? COL_ACCENT : COL_TEXT), 0);
    // With many steps the names don't fit; show only the current one.
    if (stepCount > 4) {
      if (i == step - 1) lv_obj_clear_flag(stepLabels[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(stepLabels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void set_steps(const char *const *names, int n) {
  stepCount = n > STEP_MAX ? STEP_MAX : n;
  for (int i = 0; i < stepCount; i++) {
    strncpy(stepNames[i], names[i], sizeof(stepNames[0]) - 1);
    stepNames[i][sizeof(stepNames[0]) - 1] = 0;
  }
  layout_steps();
}

static void show_run_screen(const char *file) {
  char name[64];
  display_name(name, sizeof(name), file);
  lv_label_set_text(lblRunTitle, name);
  lv_label_set_text(lblRunDesc, runDesc[0] ? runDesc : "No description available.");
  update_run_ui();
  if (lv_scr_act() != scrRun) lv_scr_load(scrRun);
}

static void stop_run() {
  running = false;
  demoRun = false;
  lv_timer_pause(runTimer);
  if (runStartTimeout) { lv_timer_del(runStartTimeout); runStartTimeout = nullptr; }
}

static void finish_run(const char *title, bool ok) {
  stop_run();
  show_result(title, ok);
}

// Standalone demo only (no Pi): fake a 4-step run.
static void run_tick_cb(lv_timer_t *t) {
  runElapsedS++;
  curStep = runElapsedS / DEMO_STEP_SECONDS + 1;
  curStepPct = (runElapsedS % DEMO_STEP_SECONDS) * 100 / DEMO_STEP_SECONDS;
  update_run_ui();
  if (runElapsedS >= runTotalS) finish_run("Run completed", true);
}

// If Start run got no RUN|... from the bridge at all within this window,
// either Klipper never entered "printing" or the bridge/Moonraker link is
// down - surface that instead of leaving the screen frozen on "Starting...".
static void run_start_timeout_cb(lv_timer_t *t) {
  runStartTimeout = nullptr;
  if (running && !demoRun && runTotalS == 0 && stepCount == 0) {
    finish_run("Run did not start", false);
  }
}

static void start_run_cb(lv_event_t *e) {
  running = true;
  runElapsedS = 0;
  curStep = 1;
  curStepPct = 0;
  // pi_connected() alone isn't enough: the bridge can be alive while the
  // list on screen is still the pre-connection placeholder (e.g. Moonraker
  // wasn't up yet when we first asked) - sending a real PRINT for a name
  // that was never a real file just gets a 404 from Klipper.
  demoRun = !pi_connected() || !haveRealFiles;
  if (!demoRun) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "CMD|PRINT|%s", selectedName);
    sendLine(cmd);
  }
  if (demoRun) {
    runTotalS = 4 * DEMO_STEP_SECONDS;
    strcpy(runDesc, "Demo run - no controller connected.");
    set_steps(DEMO_STEPS, 4);
    lv_timer_reset(runTimer);
    lv_timer_resume(runTimer);
  } else {
    // Real values arrive in the bridge's RUN line within a moment.
    runTotalS = 0;
    strcpy(runDesc, "Starting...");
    set_steps(nullptr, 0);
    if (runStartTimeout) lv_timer_del(runStartTimeout);
    runStartTimeout = lv_timer_create(run_start_timeout_cb, 8000, nullptr);
    lv_timer_set_repeat_count(runStartTimeout, 1);
  }
  show_run_screen(selectedName);
}

static void do_cancel_run() {
  sendLine("CMD|CANCEL");
  stop_run();
  goHome();
}

static void do_estop() {
  sendLine("CMD|ESTOP");
  stop_run();
  show_result("Emergency stop activated", false, /*estop_recovery=*/true);
}

static void cancel_cb(lv_event_t *e) {
  show_confirm("Cancel run?", "The current run will be stopped.", do_cancel_run);
}

static void estop_cb(lv_event_t *e) {
  show_confirm("Confirm Emergency stop", "All motion will halt immediately.", do_estop);
}

static void info_cb(lv_event_t *e) {
  char text[200] = "No step information.";
  if (stepCount > 0) {
    strcpy(text, "Steps: ");
    for (int i = 0; i < stepCount; i++) {
      strncat(text, stepNames[i], sizeof(text) - strlen(text) - 3);
      if (i < stepCount - 1) strcat(text, ", ");
    }
  }
  show_message(lv_label_get_text(lblRunTitle), text);
}

static void createRunScreen() {
  scrRun = new_screen();

  lblRunTitle = label(scrRun, "Protocol", F18, COL_TEXT);
  lv_obj_set_pos(lblRunTitle, 24, 18);
  circle_button(scrRun, 426, 14, "i", info_cb);
  lblRunDesc = label(scrRun, "", F12, COL_MUTED);
  lv_obj_set_width(lblRunDesc, 390);
  lv_obj_set_pos(lblRunDesc, 24, 50);

  lv_obj_t *tc = card(scrRun, 24, 92, 205, 140);
  lblTimerCap = label(tc, "COMPLETING IN", F12, COL_MUTED);
  lv_obj_set_style_text_letter_space(lblTimerCap, 1, 0);
  lv_obj_align(lblTimerCap, LV_ALIGN_TOP_MID, 0, 26);
  lblTimer = label(tc, "00:00", F38, COL_TIMER);
  lv_obj_align(lblTimer, LV_ALIGN_CENTER, 0, 14);

  lv_obj_set_pos(label(scrRun, "Step progress", F14, COL_MUTED), STEPS_X, 108);
  lblStepCount = label(scrRun, "", F14, COL_ACCENT);
  lv_obj_align(lblStepCount, LV_ALIGN_TOP_RIGHT, -24, 108);

  for (int i = 0; i < STEP_MAX; i++) {
    lv_obj_t *bar = lv_bar_create(scrRun);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 50, 8);
    lv_obj_set_style_bg_color(bar, C(COL_CARD_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    stepBars[i] = bar;

    stepDots[i] = box(scrRun, 0, 160, 4, 4);
    fill(stepDots[i], COL_ACCENT, LV_RADIUS_CIRCLE);
    stepLabels[i] = label(scrRun, "", F12, COL_TEXT);
  }
  set_steps(DEMO_STEPS, 4);

  lv_obj_t *cancel = button(scrRun, 24, 248, 205, 48, "Cancel", COL_CARD, F16, cancel_cb, nullptr);
  outline(cancel, COL_EDGE, 1);
  button(scrRun, 243, 248, 213, 48, "Emergency stop", COL_STOP, F16, estop_cb, nullptr);

  runTimer = lv_timer_create(run_tick_cb, 1000, nullptr);
  lv_timer_pause(runTimer);
}

// ----------------------------- LVGL plumbing ----------------------
void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  // Canvas draws into RAM; flush() pushes it to the panel over QSPI.
  gfx->flush();
  lv_disp_flush_ready(drv);
}

void my_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  touch_data_t touch_data;
  bsp_touch_read();
  if (bsp_touch_get_coordinates(&touch_data)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touch_data.coords[0].x;
    data->point.y = touch_data.coords[0].y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
  data->continue_reading = false;
}

// ----------------------------- Serial from Pi -----------------------
static void setProtocolsFromFiles(const char *files) {
  static char tmp[768];
  strncpy(tmp, files, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;

  haveRealFiles = true;
  protoCount = 0;
  for (char *p = strtok(tmp, ";"); p && protoCount < MAX_PROTOCOLS; p = strtok(nullptr, ";")) {
    while (*p == ' ') p++;
    if (!*p) continue;
    strncpy(protoNames[protoCount], p, sizeof(protoNames[0]) - 1);
    protoNames[protoCount][sizeof(protoNames[0]) - 1] = 0;
    protoCount++;
  }
  build_protocol_rows();
}

// Splits on '|' in place, keeping empty fields.
static int split_fields(char *s, char **out, int max) {
  int n = 0;
  out[n++] = s;
  for (; *s && n < max; s++) {
    if (*s == '|') {
      *s = 0;
      out[n++] = s + 1;
    }
  }
  return n;
}

// Undo the bridge's percent-encoding (%7C -> |) in place.
static void pct_decode(char *s) {
  char *o = s;
  while (*s) {
    if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char hex[3] = {s[1], s[2], 0};
      *o++ = (char)strtol(hex, nullptr, 16);
      s += 3;
    } else {
      *o++ = *s++;
    }
  }
  *o = 0;
}

// RUN|file|total_s|step1;step2;...|description
static void on_run(char **f, int n) {
  if (n < 3) return;
  strncpy(selectedName, f[1], sizeof(selectedName) - 1);
  runTotalS = strtoul(f[2], nullptr, 10);
  runDesc[0] = 0;
  if (n > 4) {
    pct_decode(f[4]);
    strncpy(runDesc, f[4], sizeof(runDesc) - 1);
  }
  const char *names[STEP_MAX];
  int count = 0;
  if (n > 3 && f[3][0]) {
    for (char *p = strtok(f[3], ";"); p && count < STEP_MAX; p = strtok(nullptr, ";")) {
      pct_decode(p);
      names[count++] = p;
    }
  }
  set_steps(names, count);

  if (runStartTimeout) { lv_timer_del(runStartTimeout); runStartTimeout = nullptr; }
  if (demoRun) lv_timer_pause(runTimer);
  demoRun = false;
  running = true;
  runElapsedS = 0;
  curStep = 1;
  curStepPct = 50;
  show_run_screen(selectedName);
}

// STATUS|state|elapsed_s|step
static void on_status(char **f, int n) {
  if (!running || demoRun || n < 3) return;
  const char *state = f[1];
  runElapsedS = strtoul(f[2], nullptr, 10);
  if (n > 3 && atoi(f[3]) > 0) curStep = atoi(f[3]);

  if (!strcmp(state, "printing") || !strcmp(state, "paused")) update_run_ui();
  else if (!strcmp(state, "complete")) finish_run("Run completed", true);
  else if (!strcmp(state, "cancelled")) finish_run("Run cancelled", false);
  else if (!strcmp(state, "error")) finish_run("Run failed", false);
}

// WIFI|1|ssid|signal|ip   or   WIFI|0
static void on_wifi(char **f, int n) {
  wifiUp = n >= 2 && f[1][0] == '1';
  if (wifiUp && n >= 5) {
    pct_decode(f[2]);
    strncpy(wifiSsid, f[2], sizeof(wifiSsid) - 1);
    wifiSignal = atoi(f[3]);
    strncpy(wifiIp, f[4], sizeof(wifiIp) - 1);
  }
  link_check_cb(nullptr);
}

static void onLineFromPi(char *line) {
#if DEBUG_SERIAL_ECHO
  Serial.print("<< ");
  Serial.println(line);
#endif
  char *f[8];
  bool known = true;

  if (!strncmp(line, "FILES|", 6)) {
    setProtocolsFromFiles(line + 6);
  } else if (!strncmp(line, "RUN|", 4)) {
    int n = split_fields(line, f, 8);
    on_run(f, n);
  } else if (!strncmp(line, "STATUS|", 7)) {
    int n = split_fields(line, f, 8);
    on_status(f, n);
  } else if (!strncmp(line, "WIFI_LIST|", 10)) {
    on_wifi_list(line + 10);
  } else if (!strncmp(line, "WIFI|", 5)) {
    int n = split_fields(line, f, 8);
    on_wifi(f, n);
  } else if (!strncmp(line, "OK|WIFI", 7)) {
    int n = split_fields(line, f, 8);
    on_wifi_result(true, n > 2 ? f[2] : nullptr);
  } else if (!strncmp(line, "ERR|WIFI|", 9)) {
    pct_decode(line + 9);
    on_wifi_result(false, line + 9);
  } else if (!strncmp(line, "OK|RECOVER", 10)) {
    on_recover_result(true, nullptr);
  } else if (!strncmp(line, "ERR|RECOVER|", 12)) {
    pct_decode(line + 12);
    on_recover_result(false, line + 12);
  } else if (!strncmp(line, "ERR|", 4)) {
    if (running && !demoRun) finish_run("Run failed", false);
  } else if (strncmp(line, "OK|", 3)) {
    known = false;
  }
  if (known) lastPiMsgMs = millis();
}

// ----------------------------- setup / loop -----------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // LCD reset via the TCA9554 expander
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  lv_init();
  screenWidth  = gfx->width();
  screenHeight = gfx->height();

  // Touch rotation must match the display rotation.
  bsp_touch_init(&Wire, -1, LCD_ROTATION, screenWidth, screenHeight);

#ifdef DIRECT_RENDER_MODE
  bufSize = screenWidth * screenHeight;
#else
  bufSize = screenWidth * 80;
#endif
  disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_8BIT);
  if (!disp_draw_buf1 || !disp_draw_buf2) {
    while (1) delay(100);
  }
  lv_disp_draw_buf_init(&draw_buf, disp_draw_buf1, disp_draw_buf2, bufSize);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
#ifdef DIRECT_RENDER_MODE
  disp_drv.full_refresh = true;
#endif
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  lastTick = millis();

  createLoadingScreen();
  lv_scr_load(scrLoading);
  pump_for(100);

  // Placeholder list until the Pi sends FILES|...
  for (int i = 0; i < 5; i++) snprintf(protoNames[i], sizeof(protoNames[0]), "Protocol %03d", i + 1);
  protoCount = 5;
  createHomeScreen();
  createListScreen();
  createWifiScreen();
  createWifiPasswordScreen();
  createDetailScreen();
  createRunScreen();

  sendLine("CMD|LIST");
  pump_for(2400);

  goHome();
  lv_obj_del(scrLoading);
  scrLoading = nullptr;
}

void loop() {
  lvgl_service();

  while (RPI_SERIAL.available()) {
    char c = (char)RPI_SERIAL.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxLine[rxPos] = 0;
      if (rxPos > 0) onLineFromPi(rxLine);
      rxPos = 0;
    } else if (rxPos < sizeof(rxLine) - 1) {
      rxLine[rxPos++] = c;
    }
  }

  delay(5);
}
