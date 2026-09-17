#include "demo.h"

#include "bsp_battery.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ppt_hid.h"
#include "ui_pixel.h"
#include <stdio.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_status;
static lv_obj_t *s_timer_label;
static lv_obj_t *s_action;
static lv_obj_t *s_battery;
static lv_obj_t *s_mascot;
static lv_timer_t *s_tick;
static ppt_hid_state_t s_last_state;
static bool s_slideshow_running;
static int64_t s_slideshow_started_us;
static uint8_t s_battery_divider;

static const char *state_text(ppt_hid_state_t state)
{
    switch (state) {
    case PPT_HID_STARTING:
        return "Starting Bluetooth...";
    case PPT_HID_ADVERTISING:
        return "PAIR IN BLUETOOTH\nDevice: PPT-Remote";
    case PPT_HID_PAIRING:
        return "PAIRING...";
    case PPT_HID_READY:
        return "CONNECTED  READY";
    case PPT_HID_FAILED:
        return "BLUETOOTH FAILED";
    default:
        return "Bluetooth stopped";
    }
}
static void refresh_timer(void)
{
    int64_t seconds = 0;
    if (s_slideshow_running) {
        seconds = (esp_timer_get_time() - s_slideshow_started_us) / 1000000;
    }
    lv_label_set_text_fmt(s_timer_label, "%02lld:%02lld",
                          (long long)(seconds / 60),
                          (long long)(seconds % 60));
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    ppt_hid_state_t state = ppt_hid_state();
    if (state != s_last_state) {
        s_last_state = state;
        if (state == PPT_HID_FAILED) {
            lv_label_set_text_fmt(s_status, "BLUETOOTH FAILED\nError: %d",
                                  ppt_hid_error());
        } else {
            lv_label_set_text(s_status, state_text(state));
        }
        if (state == PPT_HID_READY) ui_pixel_mascot_jump(s_mascot);
    }
    refresh_timer();

    if (++s_battery_divider >= 10) {
        s_battery_divider = 0;
        int soc = bsp_battery_soc();
        if (soc >= 0) lv_label_set_text_fmt(s_battery, "%d%%", soc);
        else lv_label_set_text(s_battery, "--%");
    }
}

static void queue_command(ppt_hid_command_t command, const char *action)
{
    if (ppt_hid_send(command)) {
        lv_label_set_text(s_action, action);
        ui_pixel_mascot_jump(s_mascot);
    } else {
        lv_label_set_text(s_action, "Pair Bluetooth first");
    }
}

void demo_ppt_remote_enter(void)
{
    s_scr = ui_pixel_screen_create("PPT REMOTE");

    s_battery = lv_label_create(s_scr);
    lv_obj_set_pos(s_battery, 160, 20);
    lv_obj_set_width(s_battery, 34);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(UI_INK), 0);
    lv_label_set_text(s_battery, "--%");

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 14, 54, 212, 182, UI_PAPER);
    s_status = lv_label_create(panel);
    lv_obj_set_width(s_status, 184);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(s_status, "Starting Bluetooth...");

    s_timer_label = lv_label_create(panel);
    lv_obj_set_style_text_font(s_timer_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_timer_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_timer_label, LV_ALIGN_TOP_MID, 0, 48);
    lv_label_set_text(s_timer_label, "00:00");

    lv_obj_t *controls = lv_label_create(panel);
    lv_obj_set_width(controls, 184);
    lv_obj_set_style_text_align(controls, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(controls, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(controls, lv_color_hex(UI_INK), 0);
    lv_obj_align(controls, LV_ALIGN_TOP_MID, 0, 91);
    lv_label_set_text(controls,
                      "UP: PREV    DOWN: NEXT\nOK: START   HOLD DOWN: ESC");

    s_action = lv_label_create(panel);
    lv_obj_set_width(s_action, 184);
    lv_obj_set_style_text_align(s_action, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_action, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_action, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_label_set_text(s_action, "Long OK: back");

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 242);
    s_slideshow_running = false;
    s_slideshow_started_us = 0;
    s_battery_divider = 9;
    s_last_state = PPT_HID_OFF;
    s_tick = lv_timer_create(tick, 200, NULL);
    lv_screen_load(s_scr);
    ppt_hid_start();
}

void demo_ppt_remote_exit(void)
{
    if (s_tick) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    ppt_hid_stop();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_status = s_timer_label = s_action = s_battery = s_mascot = NULL;
    s_slideshow_running = false;
}

void demo_ppt_remote_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        queue_command(PPT_HID_COMMAND_ESCAPE, "EXIT SHOW");
        s_slideshow_running = false;
        s_slideshow_started_us = 0;
        refresh_timer();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        queue_command(PPT_HID_COMMAND_PREVIOUS, "PREVIOUS SLIDE");
    } else if (btn == BSP_BTN_DOWN) {
        queue_command(PPT_HID_COMMAND_NEXT, "NEXT SLIDE");
    } else if (btn == BSP_BTN_OK) {
        if (ppt_hid_send(PPT_HID_COMMAND_START)) {
            lv_label_set_text(s_action, "START SLIDESHOW");
            s_slideshow_running = true;
            s_slideshow_started_us = esp_timer_get_time();
            ui_pixel_mascot_jump(s_mascot);
        } else {
            lv_label_set_text(s_action, "Pair Bluetooth first");
        }
    }
}
