#pragma once

#include <Arduino.h>
#include <Wire.h>

#define AXS15231B_TOUCH_ADDR 0x3B
#define TOUCH_MAX_POINTS     1

typedef struct {
  uint16_t x;
  uint16_t y;
} touch_point_t;

typedef struct {
  uint8_t touch_num;
  touch_point_t coords[TOUCH_MAX_POINTS];
} touch_data_t;

// width/height are the logical (post-rotation) screen size; rotation uses
// the same 0-3 convention as Arduino_GFX so touches line up with drawing.
void bsp_touch_init(TwoWire *wire, int tp_int, uint16_t rotation, uint16_t width, uint16_t height);
void bsp_touch_read(void);
bool bsp_touch_get_coordinates(touch_data_t *touch_data);
