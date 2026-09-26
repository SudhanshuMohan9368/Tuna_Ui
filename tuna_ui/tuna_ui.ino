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

// The Pi does not report run progress yet, so the run screen simulates it.
// Set to 0 once real progress messages exist.
#define DEMO_RUN_SIMULATION 1
#define DEMO_STEP_SECONDS   30

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

#define STEP_COUNT 4
static const char *STEP_NAMES[STEP_COUNT] = {"Binding", "Wash 1", "Wash 2", "Elution"};
static const uint32_t RUN_TOTAL_S = STEP_COUNT * DEMO_STEP_SECONDS;
static bool running = false;
static uint32_t runElapsedS = 0;
static lv_timer_t *runTimer = nullptr;

static char rxLine[768];
static size_t rxPos = 0;
static uint32_t lastTick = 0;

// ----------------------------- UI handles -------------------------
static lv_obj_t *scrLoading, *scrHome, *scrList, *scrDetail, *scrRun;
static lv_obj_t *listBox, *lblDetailTitle;
static lv_obj_t *lblRunTitle, *lblTimer, *lblStepCount;
static lv_obj_t *stepBars[STEP_COUNT], *stepLabels[STEP_COUNT];

// ----------------------------- Helpers ----------------------------
static void sendLine(const char *line) {
  RPI_SERIAL.print(line);
  RPI_SERIAL.print("\n");
  Serial.print(">> ");
  Serial.println(line);
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

static lv_obj_t *circle_button(lv_obj_t *parent, int x, int y, const char *txt, lv_event_cb_t cb) {
  lv_obj_t *b = box(parent, x, y, 30, 30);
  fill(b, COL_CARD, LV_RADIUS_CIRCLE);
  outline(b, COL_EDGE, 1);
  if (cb) make_clickable(b, cb, nullptr);
  lv_obj_center(label(b, txt, F14, COL_TEXT));
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

static void result_home_cb(lv_event_t *e) {
  close_modal_of(lv_event_get_current_target(e));
  goHome();
}

static void show_result(const char *title, bool ok) {
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
static void help_btn_cb(lv_event_t *e) { show_help_menu(); }
static void choose_protocol_cb(lv_event_t *e) { lv_scr_load(scrList); }

static void createHomeScreen() {
  scrHome = new_screen();

  lv_obj_t *brand = lv_img_create(scrHome);
  lv_img_set_src(brand, &img_cambrian_logo);
  lv_obj_set_pos(brand, 16, 16);

  circle_button(scrHome, 400, 13, LV_SYMBOL_WIFI, nullptr);
  circle_button(scrHome, 438, 13, "?", help_btn_cb);
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

static void build_protocol_rows() {
  lv_obj_clean(listBox);
  if (protoCount == 0) {
    lv_obj_set_pos(label(listBox, "No protocols found", F14, COL_MUTED), 35, 12);
    return;
  }
  for (int i = 0; i < protoCount; i++) {
    lv_obj_t *row = box(listBox, 0, i * 34, W, 34);
    make_clickable(row, proto_row_cb, (void *)(intptr_t)i);
    lv_obj_align(label(row, protoNames[i], F14, COL_TEXT), LV_ALIGN_LEFT_MID, 35, 0);
    lv_obj_align(label(row, LV_SYMBOL_RIGHT, F14, COL_TEXT), LV_ALIGN_RIGHT_MID, -24, 0);
    hline(row, 0, 33, W);
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
  lv_label_set_text(lblDetailTitle, selectedName);
  lv_scr_load(scrDetail);
}

// ----------------------------- Run screen ---------------------------
static void update_run_ui() {
  uint32_t remaining = runElapsedS < RUN_TOTAL_S ? RUN_TOTAL_S - runElapsedS : 0;
  lv_label_set_text_fmt(lblTimer, "%02lu:%02lu", (unsigned long)(remaining / 60), (unsigned long)(remaining % 60));

  int step = runElapsedS / DEMO_STEP_SECONDS;
  if (step >= STEP_COUNT) step = STEP_COUNT - 1;
  lv_label_set_text_fmt(lblStepCount, "%d of %d", step + 1, STEP_COUNT);

  for (int i = 0; i < STEP_COUNT; i++) {
    int v = 0;
    if (runElapsedS >= RUN_TOTAL_S || i < step) v = 100;
    else if (i == step) v = (runElapsedS - step * DEMO_STEP_SECONDS) * 100 / DEMO_STEP_SECONDS;
    lv_bar_set_value(stepBars[i], v, LV_ANIM_OFF);
    lv_obj_set_style_text_color(stepLabels[i], C(i == step ? COL_ACCENT : COL_TEXT), 0);
  }
}

static void stop_run() {
  running = false;
  lv_timer_pause(runTimer);
}

static void run_tick_cb(lv_timer_t *t) {
  runElapsedS++;
  update_run_ui();
  if (runElapsedS >= RUN_TOTAL_S) {
    stop_run();
    show_result("Run completed", true);
  }
}

static void start_run_cb(lv_event_t *e) {
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "CMD|PRINT|%s", selectedName);
  sendLine(cmd);

  lv_label_set_text(lblRunTitle, selectedName);
  runElapsedS = 0;
  running = true;
  update_run_ui();
  lv_scr_load(scrRun);
#if DEMO_RUN_SIMULATION
  lv_timer_reset(runTimer);
  lv_timer_resume(runTimer);
#endif
}

static void do_cancel_run() {
  sendLine("CMD|CANCEL");
  stop_run();
  goHome();
}

static void do_estop() {
  sendLine("CMD|ESTOP");
  stop_run();
  show_result("Emergency stop activated", false);
}

static void cancel_cb(lv_event_t *e) {
  show_confirm("Cancel run?", "The current run will be stopped.", do_cancel_run);
}

static void estop_cb(lv_event_t *e) {
  show_confirm("Confirm Emergency stop", "All motion will halt immediately.", do_estop);
}

static void info_cb(lv_event_t *e) {
  show_message(selectedName, "Steps: Binding, Wash 1, Wash 2, Elution.");
}

static void createRunScreen() {
  scrRun = new_screen();

  lblRunTitle = label(scrRun, "Protocol", F18, COL_TEXT);
  lv_obj_set_pos(lblRunTitle, 24, 18);
  circle_button(scrRun, 426, 14, "i", info_cb);
  lv_obj_t *desc = label(scrRun, "No description available.", F12, COL_MUTED);
  lv_obj_set_width(desc, 390);
  lv_obj_set_pos(desc, 24, 50);

  lv_obj_t *tc = card(scrRun, 24, 92, 205, 140);
  lv_obj_t *cap = label(tc, "COMPLETING IN", F12, COL_MUTED);
  lv_obj_set_style_text_letter_space(cap, 1, 0);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 26);
  lblTimer = label(tc, "00:00", F38, COL_TIMER);
  lv_obj_align(lblTimer, LV_ALIGN_CENTER, 0, 14);

  lv_obj_set_pos(label(scrRun, "Step progress", F14, COL_MUTED), 243, 108);
  lblStepCount = label(scrRun, "1 of 4", F14, COL_ACCENT);
  lv_obj_align(lblStepCount, LV_ALIGN_TOP_RIGHT, -24, 108);

  for (int i = 0; i < STEP_COUNT; i++) {
    int x = 243 + i * 54;
    lv_obj_t *bar = lv_bar_create(scrRun);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, 146);
    lv_obj_set_size(bar, 50, 8);
    lv_obj_set_style_bg_color(bar, C(COL_CARD_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    stepBars[i] = bar;

    lv_obj_t *dot = box(scrRun, x + 23, 160, 4, 4);
    fill(dot, COL_ACCENT, LV_RADIUS_CIRCLE);

    stepLabels[i] = label(scrRun, STEP_NAMES[i], F12, COL_TEXT);
    lv_obj_align_to(stepLabels[i], bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
  }

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

static void onLineFromPi(const char *line) {
  Serial.print("<< ");
  Serial.println(line);

  if (strncmp(line, "FILES|", 6) == 0) {
    setProtocolsFromFiles(line + 6);
  } else if (strncmp(line, "ERR|", 4) == 0) {
    if (running) {
      stop_run();
      show_result("Run failed", false);
    }
  }
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

#if DEMO_RUN_SIMULATION
  for (int i = 0; i < 5; i++) snprintf(protoNames[i], sizeof(protoNames[0]), "Protocol %03d", i + 1);
  protoCount = 5;
#endif
  createHomeScreen();
  createListScreen();
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
