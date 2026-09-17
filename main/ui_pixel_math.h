#pragma once

#include <stdbool.h>
#include <stdint.h>

int ui_pixel_blink_frame(uint32_t elapsed_ms);
int ui_pixel_jump_offset(unsigned frame);

int ui_pixel_battery_level(int soc);
int ui_pixel_battery_fill_w(int soc, int max_w);

#define UI_BATT_FULL_MV 4150
int ui_pixel_battery_display_soc(int soc, int mv);

typedef struct {
    uint32_t antenna;
    uint32_t face;
    uint32_t eye;
    uint32_t mouth;
    int mouth_w;
    int mouth_h;
    int bob_px;
    int bob_ms;
} ui_pixel_mascot_style_t;

const ui_pixel_mascot_style_t *ui_pixel_mascot_style(int zone, bool alarm);
