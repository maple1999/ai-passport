#include "ui_pixel_math.h"

int ui_pixel_blink_frame(uint32_t elapsed_ms)
{
    uint32_t phase = elapsed_ms % 2000U;
    return phase >= 1650U && phase < 1800U;
}

int ui_pixel_jump_offset(unsigned frame)
{
    static const int offsets[] = { 0, -3, -5, -3, 0 };
    return frame < sizeof(offsets) / sizeof(offsets[0]) ? offsets[frame] : 0;
}

int ui_pixel_battery_level(int soc)
{
    if (soc < 0) return -1;
    if (soc < 20) return 0;
    if (soc <= 50) return 1;
    return 2;
}

int ui_pixel_battery_fill_w(int soc, int max_w)
{
    if (soc < 0 || max_w <= 0) return 0;
    if (soc > 100) soc = 100;
    return (soc * max_w) / 100;
}

int ui_pixel_battery_display_soc(int soc, int mv)
{
    if (soc >= 99 && mv >= UI_BATT_FULL_MV) return 100;
    return soc;
}

static const ui_pixel_mascot_style_t MASCOT_STYLE[5] = {
    { 0x4FC3F7, 0xCFE9F5, 0x37474F, 0x90A4AE, 5, 2, 0, 0 },
    { 0x82BE2D, 0xB9F3FF, 0x294B7A, 0x7557D9, 7, 2, 0, 0 },
    { 0xFFD54F, 0xFFF3B0, 0x8D6E00, 0xFF8F00, 9, 3, 2, 1100 },
    { 0xFF9838, 0xFFE0B2, 0xE65100, 0xE65100, 11, 4, 3, 600 },
    { 0xE43B2F, 0xFFCDD2, 0xB3261E, 0xB3261E, 13, 5, 5, 350 },
};

static const ui_pixel_mascot_style_t MASCOT_STYLE_ALARM = {
    0xE43B2F, 0xFFFFFF, 0xB3261E, 0xB3261E, 13, 5, 5, 350,
};

const ui_pixel_mascot_style_t *ui_pixel_mascot_style(int zone, bool alarm)
{
    if (alarm) return &MASCOT_STYLE_ALARM;
    if (zone < 0) zone = 0;
    if (zone > 4) zone = 4;
    return &MASCOT_STYLE[zone];
}
