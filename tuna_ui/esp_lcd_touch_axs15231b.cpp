#include "esp_lcd_touch_axs15231b.h"

static TwoWire *s_wire = nullptr;
static uint16_t s_rotation = 0;
static uint16_t s_native_w = 320;
static uint16_t s_native_h = 480;
static touch_data_t s_data = {};

void bsp_touch_init(TwoWire *wire, int tp_int, uint16_t rotation, uint16_t width, uint16_t height) {
  (void)tp_int;
  s_wire = wire;
  s_rotation = rotation & 3;
  if (s_rotation & 1) {
    s_native_w = height;
    s_native_h = width;
  } else {
    s_native_w = width;
    s_native_h = height;
  }
}

void bsp_touch_read(void) {
  static const uint8_t read_cmd[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
  uint8_t buf[8] = {0};

  s_data.touch_num = 0;
  if (!s_wire) return;

  s_wire->beginTransmission(AXS15231B_TOUCH_ADDR);
  s_wire->write(read_cmd, sizeof(read_cmd));
  if (s_wire->endTransmission() != 0) return;

  if (s_wire->requestFrom((uint8_t)AXS15231B_TOUCH_ADDR, (uint8_t)sizeof(buf)) != sizeof(buf)) return;
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = s_wire->read();

  uint8_t points = buf[1];
  if (points == 0 || points > 2) return;

  uint16_t nx = ((buf[2] & 0x0F) << 8) | buf[3];
  uint16_t ny = ((buf[4] & 0x0F) << 8) | buf[5];
  if (nx >= s_native_w || ny >= s_native_h) return;

  // Inverse of Arduino_Canvas's software rotation mapping.
  uint16_t x, y;
  switch (s_rotation) {
    case 1:  x = ny;                   y = s_native_w - 1 - nx; break;
    case 2:  x = s_native_w - 1 - nx;  y = s_native_h - 1 - ny; break;
    case 3:  x = s_native_h - 1 - ny;  y = nx;                  break;
    default: x = nx;                   y = ny;                  break;
  }

  s_data.touch_num = 1;
  s_data.coords[0].x = x;
  s_data.coords[0].y = y;
}

bool bsp_touch_get_coordinates(touch_data_t *touch_data) {
  if (!touch_data || s_data.touch_num == 0) return false;
  *touch_data = s_data;
  return true;
}
