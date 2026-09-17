#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PPT_HID_OFF = 0,
    PPT_HID_STARTING,
    PPT_HID_ADVERTISING,
    PPT_HID_PAIRING,
    PPT_HID_READY,
    PPT_HID_FAILED,
} ppt_hid_state_t;

typedef enum {
    PPT_HID_COMMAND_PREVIOUS = 1,
    PPT_HID_COMMAND_NEXT,
    PPT_HID_COMMAND_START,
    PPT_HID_COMMAND_ESCAPE,
} ppt_hid_command_t;

/*
 * Owns the page-scoped NimBLE HID stack. Start and stop are called from the
 * LVGL/button path; key reports run in a worker so the button callback stays
 * non-blocking. No function in this module accesses LVGL.
 */
esp_err_t ppt_hid_start(void);
void ppt_hid_stop(void);
bool ppt_hid_send(ppt_hid_command_t command);
ppt_hid_state_t ppt_hid_state(void);
int ppt_hid_error(void);
