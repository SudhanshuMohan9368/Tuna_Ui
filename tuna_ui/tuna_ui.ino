// =============================================================
// TUNA™ Medical UI (LVGL v8) - Waveshare ESP32-S3-Touch-LCD-3.5B
// LANDSCAPE (480 x 320) build - matches the tuna_ui.py structure
//  - Home screen: status pill + Protocol card (inline dropdown)
//                 + fixed HOME / RESTART / E-STOP / RUN action row
//  - Restart: Cancel/Restart confirm dialog (lv_msgbox)
//  - E-STOP: dedicated full-screen halt view
//  - Serial bridge to Raspberry Pi Python -> Moonraker
//
// NOTE: the earlier "sample load/unload" workflow (step rail, Load/
// Unload Sample buttons, separate Protocol screen) has been removed
// on purpose to match the redesigned tuna_ui.py structure 1:1.
// =============================================================

#include <lvgl.h>
#define DIRECT_RENDER_MODE
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include <Wire.h>
#include <math.h>
#include <string.h>
#include "esp_lcd_touch_axs15231b.h"

// ----------------------------- CONFIG -----------------------------
#define RPI_SERIAL         Serial     // If using UART, change to Serial1/Serial2
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

// ----------------------------- ORIENTATION -------------------------
// IMPORTANT: the AXS15231B panel driver's own built-in `rotation`
// argument does NOT reliably support 90/270 degree rotation — this is
// a known limitation of this specific driver chip, not just this
// board. Passing rotation=1/3 straight to Arduino_AXS15231B has no
// visible effect.
//
// The fix (this is exactly what Waveshare's own official demo does):
// wrap the raw panel driver `g` in an Arduino_Canvas. Arduino_Canvas
// keeps a full software framebuffer and does the 90/270 rotation
// itself when it blits to the panel, instead of asking the AXS15231B
// chip to do it. The panel driver itself is always constructed with
// rotation=0; ALL rotation now happens via LCD_ROTATION below, passed
// to Arduino_Canvas.
//
// 1 = rotate 90° CW (landscape, connector on one side)
// 3 = rotate 270° CW (landscape, connector on the other side)
// Flip this if the image comes out upside-down for your mounting.
#define LCD_ROTATION       1   // 1 or 3 for landscape

// Native panel resolution as wired to the AXS15231B driver.
// Always pass these to Arduino_AXS15231B as (panel_w, panel_h) —
// i.e. the PORTRAIT native values — never swap them here.
#define PANEL_NATIVE_W     320
#define PANEL_NATIVE_H     480

// =============================================================
// Serial protocol (ESP32 -> Raspberry Pi bridge -> Moonraker)
// -------------------------------------------------------------
// This mirrors exactly what tuna_ui.py's Moonraker helpers do, just
// moved behind a serial line so the RPi-side bridge can make the
// actual HTTP calls. Implement these on the Pi side:
//
//   CMD|LIST              -> list_gcode_files()
//                             reply: FILES|a.gcode;b.gcode;...
//   CMD|HOME               -> home_and_park(): G28, then G0 Y<HOME_Y_MM>
//                             F<HOME_FEEDRATE> (match tuna_ui.py's
//                             HOME_Y_MM=60, HOME_FEEDRATE=3000, or
//                             whatever your machine's real park spot is)
//                             reply: OK|HOME  or  ERR|<msg>
//   CMD|RESTART            -> firmware_restart()
//                             reply: OK|RESTART  or  ERR|<msg>
//                             (ESP32 proceeds optimistically either way,
//                             same as tuna_ui.py's do_restart_confirmed)
//   CMD|ESTOP               -> emergency_stop() (fire-and-forget)
//   CMD|PRINT|<filename>    -> start_print(filename)
//                             reply: OK|PRINT  or  ERR|<msg>
// =============================================================

// ----------------------------- THEME (Easy edit) ------------------
// Calm palette (medical device)
static const uint32_t COL_BG      = 0x0B1220;  // deep navy
static const uint32_t COL_CARD    = 0x111C2E;  // card background
static const uint32_t COL_BORDER  = 0x22324A;  // subtle border
static const uint32_t COL_TEXT    = 0xE6EDF7;  // soft white
static const uint32_t COL_MUTED   = 0x9FB0C3;  // muted text

static const uint32_t COL_INFO    = 0x2B6CB0;  // calm blue — used for READY *and* RUNNING, same as tuna_ui.py's ACCENT/GREEN alias
static const uint32_t COL_WARN    = 0xF2C94C;  // amber — HOMING / RESTARTING
static const uint32_t COL_STOP    = 0xE53E3E;  // medical red — ERROR / E-STOP

static const uint32_t COL_BTN2    = 0x22324A;  // secondary button
static const uint32_t COL_BTN_TXT = 0xE6EDF7;

static const uint32_t COL_ESTOP_BG = 0x2E0F0F; // dedicated E-STOP screen background

// Fonts (keep montserrat for now)
static const lv_font_t* FONT_H1 = &lv_font_montserrat_18;
static const lv_font_t* FONT_H2 = &lv_font_montserrat_14;
static const lv_font_t* FONT_B  = &lv_font_montserrat_14;
static const lv_font_t* FONT_S  = &lv_font_montserrat_12;
static const lv_font_t* FONT_LOGO = &lv_font_montserrat_36;  // TUNA wordmark on loading screen

// ----------------------------- Layout constants (LANDSCAPE) -------
// Logical screen is now 480 wide x 320 tall (post-rotation).
static const int W = 480;
static const int H = 320;
static const int PAD = 10;
static const int RADIUS = 10;

static const int HDR_H     = 42;                 // header height
static const int CONTENT_Y = HDR_H + 8;           // top of the Protocol card

// Bottom action row: HOME | RESTART | E-STOP | RUN(wide)
static const int ACTION_ROW_H  = 64;
static const int ACTION_ROW_Y  = H - PAD - ACTION_ROW_H;
static const int ACTION_GAP    = 8;
static const int ACTION_SMALL_W = 88;
static const int ACTION_X0     = PAD;
static const int ACTION_X1     = ACTION_X0 + ACTION_SMALL_W + ACTION_GAP;
static const int ACTION_X2     = ACTION_X1 + ACTION_SMALL_W + ACTION_GAP;
static const int ACTION_X3     = ACTION_X2 + ACTION_SMALL_W + ACTION_GAP;
static const int ACTION_RUN_W  = W - PAD - ACTION_X3;

// ----------------------------- Display objects --------------------
TCA9554 TCA(0x20);
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_QSPI_CS, LCD_QSPI_CLK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
// Raw panel driver: ALWAYS rotation=0 here, native portrait dims.
// Do NOT put LCD_ROTATION on this line — the AXS15231B chip itself
// doesn't rotate reliably for 90/270; see note above.
Arduino_GFX *g = new Arduino_AXS15231B(bus, -1, 0, false, PANEL_NATIVE_W, PANEL_NATIVE_H);
// Software-rotated canvas wrapper. This is what the rest of the sketch
// (and LVGL) actually talks to. gfx->width()/height() now correctly
// report 480x320 and the rotation is applied reliably in software.
Arduino_Canvas *gfx = new Arduino_Canvas(PANEL_NATIVE_W, PANEL_NATIVE_H, g, 0, 0, LCD_ROTATION);

uint32_t screenWidth, screenHeight, bufSize;
lv_disp_draw_buf_t draw_buf;
lv_color_t *disp_draw_buf1;
lv_color_t *disp_draw_buf2;
lv_disp_drv_t disp_drv;

// ----------------------------- App state --------------------------
// Simplified to match tuna_ui.py's AppState: ready | homing | printing |
// error, plus a separate `estopped` flag (E-STOP is orthogonal to the
// normal state machine, same as Python's STATE.estopped bool).
enum UiState {
  ST_BOOT,
  ST_READY,
  ST_HOMING,
  ST_RUNNING,
  ST_ERROR
};

static UiState state = ST_BOOT;
static bool estopped = false;

static bool fileSelected = false;
static bool haveFiles = false;
static char selectedFile[128] = {0};

// Serial RX line buffer
static char rxLine[768];
static size_t rxPos = 0;

// ----------------------------- UI handles -------------------------
static lv_obj_t *scrHome = nullptr;
static lv_obj_t *scrLoading = nullptr;
static lv_obj_t *scrEstop = nullptr;

// Loading screen widgets
static lv_obj_t *loadingBar = nullptr;
static lv_obj_t *lblLoadingCaption = nullptr;
static lv_obj_t *lblLoadingPercent = nullptr;

// Header / status pill
static lv_obj_t *statusPill = nullptr;
static lv_obj_t *statusDot = nullptr;
static lv_obj_t *lblStatus = nullptr;

// Protocol card
static lv_obj_t *protoDropdown = nullptr;

// Action row buttons
static lv_obj_t *btnHome = nullptr;
static lv_obj_t *btnRestart = nullptr;
static lv_obj_t *btnEStop = nullptr;
static lv_obj_t *btnRun = nullptr;

// ----------------------------- Helpers ----------------------------
static void sendLine(const char *line) {
  RPI_SERIAL.print(line);
  RPI_SERIAL.print("\n");
  // Debug to USB serial console
  Serial.print(">> ");
  Serial.println(line);
}

static lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

static void style_card(lv_obj_t *o) {
  lv_obj_set_style_bg_color(o, C(COL_CARD), 0);
  lv_obj_set_style_border_color(o, C(COL_BORDER), 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, RADIUS, 0);
  lv_obj_set_style_pad_all(o, 10, 0);
}

static void style_btn(lv_obj_t *b, uint32_t bg) {
  lv_obj_set_style_bg_color(b, C(bg), 0);
  lv_obj_set_style_radius(b, RADIUS, 0);
  lv_obj_set_style_border_width(b, 0, 0);
}

static void set_label(lv_obj_t *lbl, const char *txt, const lv_font_t *f, uint32_t col) {
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, f, 0);
  lv_obj_set_style_text_color(lbl, C(col), 0);
}

static lv_obj_t* make_card(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  style_card(c);
  return c;
}

// ----------------------------- Status pill -------------------------
static void setStatusPill(const char *text, uint32_t color) {
  lv_label_set_text(lblStatus, text);
  lv_obj_set_style_text_color(lblStatus, C(color), 0);
  lv_obj_set_style_bg_color(statusDot, C(color), 0);
  lv_obj_set_style_border_color(statusPill, C(color), 0);
}

// ----------------------------- RUN button ---------------------------
// Mirrors tuna_ui.py's draw_home() RUN-button logic:
//   ready   = selected file AND not currently running -> filled accent
//   running = currently running                       -> dim outline, accent text
//   else (nothing selected)                            -> disabled-looking dim
static void updateRunButtonUI() {
  bool running = (state == ST_RUNNING);
  bool ready = fileSelected && !running;

  lv_obj_t *lbl = lv_obj_get_child(btnRun, 0);
  lv_label_set_text(lbl, running ? "RUNNING" : "RUN");

  if (ready) {
    style_btn(btnRun, COL_INFO);
    lv_obj_set_style_text_color(lbl, C(COL_BTN_TXT), 0);
  } else if (running) {
    style_btn(btnRun, COL_BTN2);
    lv_obj_set_style_text_color(lbl, C(COL_INFO), 0);
  } else {
    style_btn(btnRun, COL_BORDER);
    lv_obj_set_style_text_color(lbl, C(COL_MUTED), 0);
  }
}

// State-driven UI update
static void applyState() {
  switch (state) {
    case ST_BOOT:    setStatusPill("STARTING", COL_WARN); break;
    case ST_READY:   setStatusPill("READY",    COL_INFO); break;
    case ST_HOMING:  setStatusPill("HOMING",   COL_WARN); break;
    case ST_RUNNING: setStatusPill("RUNNING",  COL_INFO); break;
    case ST_ERROR:   setStatusPill("ERROR",    COL_STOP); break;
  }
  updateRunButtonUI();
}

// ----------------------------- Screens ----------------------------
static void goHome() {
  if (scrHome) lv_scr_load(scrHome);
}

static void goLoading() {
  if (scrLoading) lv_scr_load(scrLoading);
}

static void goEstop() {
  if (scrEstop) lv_scr_load(scrEstop);
}

// ----------------------------- Protocol dropdown --------------------
// Builds the dropdown's option list from a FILES|... payload
// (semicolon separated), mirroring tuna_ui.py's list_gcode_files() ->
// dropdown population. Index 0 is always the "Select protocol"
// placeholder, matching the Python version's unselected state.
static void setProtocolsFromFiles(const char *files) {
  static char opts[1200];
  strcpy(opts, "Select protocol");

  if (files && files[0]) {
    haveFiles = true;
    static char tmp[1200];
    strncpy(tmp, files, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;

    char *p = tmp;
    while (p && *p) {
      char *sep = strchr(p, ';');
      if (sep) *sep = 0;

      while (*p == ' ') p++;
      if (*p) {
        strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
        strncat(opts, p, sizeof(opts) - strlen(opts) - 1);
      }

      if (!sep) break;
      p = sep + 1;
    }
  } else {
    haveFiles = false;
    strncat(opts, "\n(no files found)", sizeof(opts) - strlen(opts) - 1);
  }

  lv_dropdown_set_options(protoDropdown, opts);
  lv_dropdown_set_selected(protoDropdown, 0);
  fileSelected = false;
  selectedFile[0] = 0;
  updateRunButtonUI();
}

static void dropdown_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

  uint16_t sel = lv_dropdown_get_selected(protoDropdown);
  char buf[128];
  lv_dropdown_get_selected_str(protoDropdown, buf, sizeof(buf));

  if (sel == 0 || strcmp(buf, "(no files found)") == 0) {
    fileSelected = false;
    selectedFile[0] = 0;
  } else {
    fileSelected = true;
    strncpy(selectedFile, buf, sizeof(selectedFile) - 1);
    selectedFile[sizeof(selectedFile) - 1] = 0;
  }
  updateRunButtonUI();
}

// Styles the dropdown's popup list only when it's genuinely open. LVGL
// only creates the list object while open, and sends LV_EVENT_READY on
// the dropdown once that list exists — this is the documented safe hook
// for this, unlike manually forcing open()+close().
static void dropdown_ready_event(lv_event_t *e) {
  lv_obj_t *dd = lv_event_get_target(e);
  lv_obj_t *list = lv_dropdown_get_list(dd);
  if (!list) return;

  lv_obj_set_style_bg_color(list, C(COL_CARD), 0);
  lv_obj_set_style_text_color(list, C(COL_TEXT), 0);
  lv_obj_set_style_border_color(list, C(COL_BORDER), 0);
  lv_obj_set_style_border_width(list, 1, 0);
  lv_obj_set_style_text_font(list, FONT_B, 0);
  lv_obj_set_style_bg_color(list, C(COL_INFO), LV_PART_SELECTED);
}

// ----------------------------- Actions -----------------------------
static void doHome() {
  state = ST_HOMING;
  applyState();
  sendLine("CMD|HOME");
}

static void doRun() {
  if (!fileSelected || state == ST_RUNNING) return;
  state = ST_RUNNING;
  applyState();

  char cmd[220];
  snprintf(cmd, sizeof(cmd), "CMD|PRINT|%s", selectedFile);
  sendLine(cmd);
}

static void doEstop() {
  sendLine("CMD|ESTOP");
  estopped = true;
  state = ST_ERROR;
  goEstop();
}

// Restart confirm dialog (Cancel / Restart), mirrors
// tuna_ui.py's draw_confirm_restart() + do_restart_confirmed().
static void restart_msgbox_event(lv_event_t *e) {
  lv_obj_t *mbox = lv_event_get_current_target(e);
  const char *txt = lv_msgbox_get_active_btn_text(mbox);

  if (txt && strcmp(txt, "Restart") == 0) {
    setStatusPill("RESTARTING", COL_WARN);
    lv_timer_handler();
    sendLine("CMD|RESTART");
    delay(500);   // brief, matches the short pause in do_restart_confirmed()
    state = ST_READY;
    applyState();
  }
  lv_msgbox_close(mbox);
}

static void showRestartConfirm() {
  static const char *btns[] = {"Cancel", "Restart", ""};
  lv_obj_t *mbox = lv_msgbox_create(NULL, "Restart Klipper?",
      "Firmware will restart. Any running protocol will be interrupted.",
      btns, false);
  lv_obj_set_style_bg_color(mbox, C(COL_CARD), 0);
  lv_obj_set_style_text_color(mbox, C(COL_TEXT), 0);
  lv_obj_center(mbox);
  lv_obj_add_event_cb(mbox, restart_msgbox_event, LV_EVENT_VALUE_CHANGED, NULL);
}

// ----------------------------- Button events ------------------------
static void btn_home_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (estopped) return;
  doHome();
}

static void btn_restart_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (estopped) return;
  showRestartConfirm();
}

static void btn_estop_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  doEstop();
}

static void btn_run_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (estopped) return;
  doRun();
}

static void btn_estop_restart_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  sendLine("CMD|RESTART");
  estopped = false;
  state = ST_READY;
  applyState();
  goHome();
}

// ----------------------------- TUNA wordmark -----------------------
// Draws "TUN" with the given font, then adds a plain two-stroke "A" -
// an upside-down V with NO crossbar - as an lv_line polyline glyph
// instead of relying on the font's default A. Returns the row
// container so callers can lv_obj_align_to() other elements (like a
// version tag) against the whole wordmark.
static lv_obj_t* make_tuna_wordmark(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int a_w, int a_h, int stroke) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 4, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lblTUN = lv_label_create(row);
  set_label(lblTUN, "TUN", font, color);

  // The "A": just two diagonal strokes meeting at an apex, no crossbar.
  static lv_point_t a_pts[3];
  a_pts[0] = { 0,      a_h };
  a_pts[1] = { a_w / 2, 0  };
  a_pts[2] = { a_w,    a_h };

  lv_obj_t *aLine = lv_line_create(row);
  lv_obj_set_size(aLine, a_w, a_h);
  lv_line_set_points(aLine, a_pts, 3);
  lv_obj_set_style_line_width(aLine, stroke, 0);
  lv_obj_set_style_line_color(aLine, C(color), 0);
  lv_obj_set_style_line_rounded(aLine, true, 0);

  return row;
}

// ----------------------------- Loading screen -----------------------
static void createLoadingScreen() {
  scrLoading = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrLoading, C(COL_BG), 0);
  lv_obj_set_style_border_width(scrLoading, 0, 0);

  // "CAMBRIAN BIOWORKS" eyebrow — letter-spaced caps, muted, centered.
  // Placeholder text; swap for lv_img_create() + your logo PNG/SVG
  // (converted to a C array with LVGL's image converter) once you have
  // the brand asset.
  lv_obj_t *lblEyebrow = lv_label_create(scrLoading);
  set_label(lblEyebrow, "CAMBRIAN BIOWORKS", FONT_S, COL_MUTED);
  lv_obj_set_style_text_letter_space(lblEyebrow, 4, 0);
  lv_obj_align(lblEyebrow, LV_ALIGN_TOP_MID, 0, 78);

  // "TUNA" wordmark with the custom crossbar-less "A".
  lv_obj_t *logoRow = make_tuna_wordmark(scrLoading, FONT_LOGO, COL_TEXT, 30, 26, 5);
  lv_obj_align(logoRow, LV_ALIGN_TOP_MID, 0, 108);

  // "v1.0" tag, aligned to the top-right corner of the wordmark.
  lv_obj_t *lblVersion = lv_label_create(scrLoading);
  set_label(lblVersion, "v1.0", FONT_S, COL_INFO);
  lv_obj_align_to(lblVersion, logoRow, LV_ALIGN_OUT_RIGHT_TOP, 6, 2);

  // Progress bar
  loadingBar = lv_bar_create(scrLoading);
  lv_obj_set_size(loadingBar, W - 160, 10);
  lv_obj_align(loadingBar, LV_ALIGN_TOP_MID, 0, 190);
  lv_bar_set_range(loadingBar, 0, 100);
  lv_bar_set_value(loadingBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(loadingBar, C(COL_BORDER), LV_PART_MAIN);
  lv_obj_set_style_radius(loadingBar, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(loadingBar, C(COL_INFO), LV_PART_INDICATOR);
  lv_obj_set_style_radius(loadingBar, 5, LV_PART_INDICATOR);

  // "Loading protocols" (left) ... "NN%" (right), same row under the bar
  lblLoadingCaption = lv_label_create(scrLoading);
  set_label(lblLoadingCaption, "Loading protocols", FONT_S, COL_MUTED);
  lv_obj_align_to(lblLoadingCaption, loadingBar, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

  lblLoadingPercent = lv_label_create(scrLoading);
  set_label(lblLoadingPercent, "0%", FONT_S, COL_MUTED);
  lv_obj_align_to(lblLoadingPercent, loadingBar, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 8);
}

// Animate the loading bar 0->100 with an ease-out curve, pumping LVGL's
// timer handler each frame so the display actually updates (this runs
// before the main loop() starts, so nothing else is servicing LVGL).
static void runLoadingAnimation() {
  goLoading();
  const int steps = 30;
  for (int i = 0; i <= steps; i++) {
    float t = (float)i / steps;
    float eased = 1.0f - powf(1.0f - t, 3.0f);   // ease-out cubic, matches the Python version
    int percent = (int)(eased * 100.0f);

    lv_bar_set_value(loadingBar, percent, LV_ANIM_OFF);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(lblLoadingPercent, buf);

    lv_timer_handler();
    delay(12);
  }
}

// ----------------------------- Home screen ---------------------------
static void createHomeScreen() {
  scrHome = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrHome, C(COL_BG), 0);
  lv_obj_set_style_border_width(scrHome, 0, 0);

  // ---------------- Header: page label | status pill | brand --------
  lv_obj_t *hdr = lv_obj_create(scrHome);
  lv_obj_set_size(hdr, W, HDR_H);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, C(COL_BG), 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_pad_hor(hdr, PAD, 0);
  lv_obj_set_style_pad_ver(hdr, 0, 0);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lblPage = lv_label_create(hdr);
  set_label(lblPage, "HOME", FONT_H2, COL_TEXT);

  // Status pill (colored dot + label), matches tuna_ui.py's top-right
  // READY/HOMING/RUNNING/ERROR pill.
  statusPill = lv_obj_create(hdr);
  lv_obj_set_size(statusPill, 96, 26);
  lv_obj_set_style_bg_color(statusPill, C(COL_CARD), 0);
  lv_obj_set_style_border_color(statusPill, C(COL_INFO), 0);
  lv_obj_set_style_border_width(statusPill, 1, 0);
  lv_obj_set_style_radius(statusPill, 13, 0);
  lv_obj_set_style_pad_all(statusPill, 0, 0);
  lv_obj_clear_flag(statusPill, LV_OBJ_FLAG_SCROLLABLE);

  statusDot = lv_obj_create(statusPill);
  lv_obj_set_size(statusDot, 8, 8);
  lv_obj_set_style_radius(statusDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(statusDot, 0, 0);
  lv_obj_align(statusDot, LV_ALIGN_LEFT_MID, 10, 0);

  lblStatus = lv_label_create(statusPill);
  set_label(lblStatus, "READY", FONT_S, COL_INFO);
  lv_obj_align(lblStatus, LV_ALIGN_LEFT_MID, 24, 0);

  lv_obj_t *lblBrand = lv_label_create(hdr);
  set_label(lblBrand, "TUNA", FONT_H1, COL_INFO);

  // ---------------- Protocol card (title + inline dropdown) ----------
  int cardY = CONTENT_Y;
  int cardH = ACTION_ROW_Y - 8 - cardY;
  lv_obj_t *card = make_card(scrHome, PAD, cardY, W - 2 * PAD, cardH);

  lv_obj_t *lblProtoTitle = lv_label_create(card);
  set_label(lblProtoTitle, "Protocol", FONT_H1, COL_TEXT);
  lv_obj_align(lblProtoTitle, LV_ALIGN_TOP_LEFT, 0, -2);

  lv_obj_t *lblProtoSub = lv_label_create(card);
  set_label(lblProtoSub, "Klipper gcode file to run", FONT_S, COL_MUTED);
  lv_obj_align(lblProtoSub, LV_ALIGN_TOP_LEFT, 0, 26);

  protoDropdown = lv_dropdown_create(card);
  lv_dropdown_set_options(protoDropdown, "Select protocol");
  lv_obj_set_size(protoDropdown, W - 2 * PAD - 20, 42);
  lv_obj_align(protoDropdown, LV_ALIGN_TOP_LEFT, 0, 58);
  lv_obj_set_style_bg_color(protoDropdown, C(COL_CARD), 0);
  lv_obj_set_style_border_color(protoDropdown, C(COL_BORDER), 0);
  lv_obj_set_style_border_width(protoDropdown, 1, 0);
  lv_obj_set_style_text_color(protoDropdown, C(COL_TEXT), 0);
  lv_obj_set_style_text_font(protoDropdown, FONT_B, 0);
  lv_obj_add_event_cb(protoDropdown, dropdown_event, LV_EVENT_VALUE_CHANGED, NULL);

  // Style the popup list the safe way: LV_EVENT_READY fires each time the
  // dropdown genuinely finishes opening, so we style it in place rather
  // than forcing an artificial open()+close() cycle (that hack corrupts
  // the dropdown's open/close animation state and was causing a
  // StoreProhibited crash on boot — a dangling animation callback fired
  // later, during the loading-screen animation loop, into freed memory).
  lv_obj_add_event_cb(protoDropdown, dropdown_ready_event, LV_EVENT_READY, NULL);

  // ---------------- Bottom action row: HOME | RESTART | E-STOP | RUN -
  btnHome = lv_btn_create(scrHome);
  lv_obj_set_size(btnHome, ACTION_SMALL_W, ACTION_ROW_H);
  lv_obj_set_pos(btnHome, ACTION_X0, ACTION_ROW_Y);
  style_btn(btnHome, COL_BTN2);
  lv_obj_add_event_cb(btnHome, btn_home_event, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lblHomeBtn = lv_label_create(btnHome);
  set_label(lblHomeBtn, "HOME", FONT_S, COL_TEXT);
  lv_obj_center(lblHomeBtn);

  btnRestart = lv_btn_create(scrHome);
  lv_obj_set_size(btnRestart, ACTION_SMALL_W, ACTION_ROW_H);
  lv_obj_set_pos(btnRestart, ACTION_X1, ACTION_ROW_Y);
  style_btn(btnRestart, COL_CARD);
  lv_obj_set_style_border_width(btnRestart, 1, 0);
  lv_obj_set_style_border_color(btnRestart, C(COL_WARN), 0);
  lv_obj_add_event_cb(btnRestart, btn_restart_event, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lblRestartBtn = lv_label_create(btnRestart);
  set_label(lblRestartBtn, "RESTART", FONT_S, COL_WARN);
  lv_obj_center(lblRestartBtn);

  btnEStop = lv_btn_create(scrHome);
  lv_obj_set_size(btnEStop, ACTION_SMALL_W, ACTION_ROW_H);
  lv_obj_set_pos(btnEStop, ACTION_X2, ACTION_ROW_Y);
  style_btn(btnEStop, COL_STOP);
  lv_obj_add_event_cb(btnEStop, btn_estop_event, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lblEStopBtn = lv_label_create(btnEStop);
  set_label(lblEStopBtn, "E-STOP", FONT_S, COL_BTN_TXT);
  lv_obj_center(lblEStopBtn);

  btnRun = lv_btn_create(scrHome);
  lv_obj_set_size(btnRun, ACTION_RUN_W, ACTION_ROW_H);
  lv_obj_set_pos(btnRun, ACTION_X3, ACTION_ROW_Y);
  style_btn(btnRun, COL_BORDER);
  lv_obj_add_event_cb(btnRun, btn_run_event, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lblRunBtn = lv_label_create(btnRun);
  set_label(lblRunBtn, "RUN", FONT_B, COL_MUTED);
  lv_obj_center(lblRunBtn);
}

// ----------------------------- E-STOP screen -------------------------
static void createEstopScreen() {
  scrEstop = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrEstop, C(COL_ESTOP_BG), 0);
  lv_obj_set_style_border_width(scrEstop, 0, 0);

  lv_obj_t *lblTitle = lv_label_create(scrEstop);
  set_label(lblTitle, "EMERGENCY STOP", FONT_H1, COL_STOP);
  lv_obj_align(lblTitle, LV_ALIGN_TOP_MID, 0, 100);

  lv_obj_t *lblMsg = lv_label_create(scrEstop);
  set_label(lblMsg, "Machine halted. Clear the fault, then restart.", FONT_S, COL_MUTED);
  lv_obj_align(lblMsg, LV_ALIGN_TOP_MID, 0, 132);

  lv_obj_t *btnRestartEstop = lv_btn_create(scrEstop);
  lv_obj_set_size(btnRestartEstop, 220, 46);
  lv_obj_align(btnRestartEstop, LV_ALIGN_TOP_MID, 0, 176);
  style_btn(btnRestartEstop, COL_STOP);
  lv_obj_add_event_cb(btnRestartEstop, btn_estop_restart_event, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lblR = lv_label_create(btnRestartEstop);
  set_label(lblR, "RESTART", FONT_B, COL_BTN_TXT);
  lv_obj_center(lblR);
}

// ----------------------------- LVGL plumbing ----------------------
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  // Arduino_Canvas only draws into its internal software framebuffer;
  // flush() is what actually pushes that buffer out to the physical
  // panel over QSPI. Without this call the screen never updates.
  gfx->flush();
  lv_disp_flush_ready(disp_drv);
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

// ----------------------------- Incoming serial parse --------------
static void onLineFromPi(const char *line) {
  // FILES|a.gcode;b.gcode;...
  if (strncmp(line, "FILES|", 6) == 0) {
    setProtocolsFromFiles(line + 6);
    return;
  }

  if (strncmp(line, "OK|HOME", 7) == 0) {
    if (state == ST_HOMING) {
      state = ST_READY;
      applyState();
    }
    return;
  }

  if (strncmp(line, "OK|PRINT", 8) == 0) {
    // Already showing RUNNING; nothing else to do.
    return;
  }

  if (strncmp(line, "OK|RESTART", 10) == 0) {
    // Handled optimistically in restart_msgbox_event(); nothing to do.
    return;
  }

  if (strncmp(line, "ERR|", 4) == 0) {
    if (state == ST_HOMING || state == ST_RUNNING) {
      state = ST_ERROR;
      applyState();
    }
    return;
  }

  // debug
  Serial.print("<< ");
  Serial.println(line);
}

// ----------------------------- setup / loop -----------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  // I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // TCA reset toggle (board-specific)
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);

  // Display
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  // LVGL init
  lv_init();

  // These now come back as 480 x 320 because of LCD_ROTATION above.
  screenWidth  = gfx->width();
  screenHeight = gfx->height();

  // Touch rotation MUST match the display rotation, or touches will
  // land at swapped/mirrored coordinates relative to what's drawn.
  // bsp_touch_init(wire, int_pin, rotation, width, height)
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

  // Create screens
  createLoadingScreen();
  createHomeScreen();
  createEstopScreen();

  // Boot: show the TUNA loading screen with an animated progress bar
  // before dropping into Home.
  runLoadingAnimation();

  // Initial state
  state = ST_READY;
  applyState();
  goHome();

  // Ask for files on boot
  sendLine("CMD|LIST");
}

static uint32_t lastTick = 0;

void loop() {
  // LVGL tick
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;
  lv_timer_handler();

  // Serial read line-based
  while (RPI_SERIAL.available()) {
    char c = (char)RPI_SERIAL.read();
    if (c == '\r') continue;

    if (c == '\n') {
      rxLine[rxPos] = 0;
      if (rxPos > 0) onLineFromPi(rxLine);
      rxPos = 0;
    } else {
      if (rxPos < sizeof(rxLine) - 1) rxLine[rxPos++] = c;
    }
  }

  delay(5);
}
