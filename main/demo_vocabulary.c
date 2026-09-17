#include "demo.h"
#include "vocabulary_client.h"
#include "vocabulary_model.h"

#include "bsp_battery.h"
#include "font_source_han_sans_sc_14_gb2312.h"
#include "ui_pixel.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_vocabulary";

#define VOCAB_WORKER_STACK 10240
#define VOCAB_STATUS_BYTES 96
#define VOCAB_NVS_NAMESPACE "vocab_ui"

typedef enum {
    VOCAB_CMD_START = 1,
    VOCAB_CMD_SUBMIT,
    VOCAB_CMD_FINISH,
    VOCAB_CMD_AUDIO_SETTING,
} vocabulary_command_type_t;

typedef struct {
    vocabulary_command_type_t type;
    char session_id[VOCABULARY_SESSION_ID_BYTES];
    char card_id[VOCABULARY_ID_BYTES];
    char idempotency_key[80];
    vocabulary_grade_t grade;
    bool enabled;
} vocabulary_command_t;

typedef enum {
    VOCAB_UI_STATUS = 1,
    VOCAB_UI_SESSION,
    VOCAB_UI_SYNC,
    VOCAB_UI_STOPPED,
} vocabulary_ui_type_t;

typedef struct {
    vocabulary_ui_type_t type;
    esp_err_t error;
    char status[VOCAB_STATUS_BYTES];
    vocabulary_session_t session;
    bool auto_pronunciation;
} vocabulary_ui_message_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_word;
static lv_obj_t *s_phonetic;
static lv_obj_t *s_meaning;
static lv_obj_t *s_progress;
static lv_obj_t *s_status;
static lv_obj_t *s_audio;
static lv_obj_t *s_battery;
static lv_obj_t *s_mascot;
static lv_timer_t *s_timer;
static QueueHandle_t s_command_queue;
static QueueHandle_t s_ui_queue;
static TaskHandle_t s_worker_task;
static vocabulary_model_t s_model;
static char s_session_id[VOCABULARY_SESSION_ID_BYTES];
static volatile bool s_worker_busy;
static volatile bool s_client_open;
static volatile bool s_finish_requested;
static bool s_page_active;
static uint32_t s_event_sequence;

static bool preference_load(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(VOCAB_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_get_u8(handle, "auto_audio", &value);
    nvs_close(handle);
    return err == ESP_OK && value != 0;
}
static void preference_store(bool enabled)
{
    nvs_handle_t handle;
    if (nvs_open(VOCAB_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, "auto_audio", enabled ? 1 : 0) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static void publish_status(vocabulary_ui_type_t type, esp_err_t error,
                           const char *status)
{
    vocabulary_ui_message_t message = { .type = type, .error = error };
    if (status) strlcpy(message.status, status, sizeof(message.status));
    if (s_ui_queue) xQueueOverwrite(s_ui_queue, &message);
}

static void worker_task(void *arg)
{
    (void)arg;
    vocabulary_command_t command;
    for (;;) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        s_worker_busy = true;
        if (command.type == VOCAB_CMD_START) {
            publish_status(VOCAB_UI_STATUS, ESP_OK, "正在连接手机热点…");
            vocabulary_ui_message_t message = { .type = VOCAB_UI_SESSION };
            message.auto_pronunciation = preference_load();
            esp_err_t err = vocabulary_client_open(&message.session);
            message.error = err;
            if (err == ESP_OK) {
                s_client_open = true;
                strlcpy(message.status,
                        message.session.daily_complete ? "今日任务已完成" : "已连接 TXyun",
                        sizeof(message.status));
            } else {
                strlcpy(message.status,
                        err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_STATE
                            ? "请先通过 USB 配置热点和服务器"
                            : "连接失败，请检查热点与服务器",
                        sizeof(message.status));
            }
            xQueueOverwrite(s_ui_queue, &message);
        } else if (command.type == VOCAB_CMD_SUBMIT) {
            esp_err_t err = ESP_FAIL;
            for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
                err = vocabulary_client_submit(command.session_id, command.card_id,
                                               command.grade,
                                               command.idempotency_key);
                if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(500u << attempt));
            }
            publish_status(VOCAB_UI_SYNC, err,
                           err == ESP_OK ? "学习进度已同步" : "同步失败；本次会话请勿断电");
        } else if (command.type == VOCAB_CMD_AUDIO_SETTING) {
            preference_store(command.enabled);
        } else if (command.type == VOCAB_CMD_FINISH) {
            esp_err_t err = ESP_OK;
            if (s_client_open && command.session_id[0]) {
                err = vocabulary_client_finish(command.session_id);
            }
            vocabulary_client_close();
            s_client_open = false;
            publish_status(VOCAB_UI_STOPPED, err,
                           err == ESP_OK ? "会话已结束，请再次长按 OK 返回"
                                         : "已离线结束，请再次长按 OK 返回");
        }
        s_worker_busy = false;
    }
}

static void ensure_worker(void)
{
    if (!s_command_queue) s_command_queue = xQueueCreate(8, sizeof(vocabulary_command_t));
    if (!s_ui_queue) s_ui_queue = xQueueCreate(1, sizeof(vocabulary_ui_message_t));
    if (!s_worker_task && s_command_queue && s_ui_queue) {
        if (xTaskCreate(worker_task, "vocab_worker", VOCAB_WORKER_STACK, NULL, 4,
                        &s_worker_task) != pdPASS) {
            s_worker_task = NULL;
            ESP_LOGE(TAG, "词汇工作任务创建失败");
        }
    }
}

static void refresh_card(void)
{
    const vocabulary_card_t *card = vocabulary_model_current(&s_model);
    if (!card) {
        lv_label_set_text(s_word, "DONE");
        lv_label_set_text(s_phonetic, "");
        lv_label_set_text(s_meaning, "今天的学习任务已完成");
        lv_label_set_text(s_progress, "0 / 0");
        return;
    }
    lv_label_set_text(s_word, card->word);
    lv_label_set_text(s_phonetic, card->phonetic);
    lv_label_set_text(s_meaning,
                      s_model.meaning_visible ? card->meaning : "按 DOWN 查看释义");
    lv_label_set_text_fmt(s_progress, "%u / %u", (unsigned)(s_model.index + 1),
                          (unsigned)s_model.count);
    lv_label_set_text(s_audio, s_model.auto_pronunciation ? "AUDIO ON*" : "AUDIO OFF");
}

static void apply_message(const vocabulary_ui_message_t *message)
{
    if (!s_page_active || !s_status) return;
    lv_label_set_text(s_status, message->status);
    if (message->type == VOCAB_UI_SESSION) {
        if (message->error == ESP_OK) {
            strlcpy(s_session_id, message->session.session_id, sizeof(s_session_id));
            vocabulary_model_init(&s_model, message->session.cards,
                                  message->session.card_count,
                                  message->auto_pronunciation);
            refresh_card();
            s_finish_requested = false;
        } else {
            lv_label_set_text(s_word, "SETUP");
            lv_label_set_text(s_phonetic, "tools/configure_vocabulary.py");
            lv_label_set_text(s_meaning, "通过 USB 写入手机热点、服务地址与配对码");
        }
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    vocabulary_ui_message_t message;
    if (s_ui_queue && xQueueReceive(s_ui_queue, &message, 0) == pdTRUE) {
        apply_message(&message);
    }
    static uint8_t battery_divider;
    if (++battery_divider >= 20) {
        battery_divider = 0;
        int soc = bsp_battery_soc();
        lv_label_set_text_fmt(s_battery, soc >= 0 ? "%d%%" : "--", soc);
    }
}

static void queue_result(const vocabulary_action_t *action)
{
    if (!action->submit || action->card_index >= s_model.count || !s_client_open) return;
    vocabulary_command_t command = { .type = VOCAB_CMD_SUBMIT, .grade = action->grade };
    strlcpy(command.session_id, s_session_id, sizeof(command.session_id));
    strlcpy(command.card_id, s_model.cards[action->card_index].id,
            sizeof(command.card_id));
    s_event_sequence++;
    snprintf(command.idempotency_key, sizeof(command.idempotency_key), "%s-%lu-%c",
             command.card_id, (unsigned long)s_event_sequence,
             action->grade == VOCABULARY_GRADE_KNOWN ? 'k' : 'a');
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        lv_label_set_text(s_status, "同步队列已满，请稍候");
    } else {
        lv_label_set_text(s_status,
                          action->grade == VOCABULARY_GRADE_KNOWN ? "已标记：认识"
                                                                  : "已标记：不熟悉");
    }
}

void demo_vocabulary_enter(void)
{
    ensure_worker();
    s_page_active = true;
    s_finish_requested = false;
    s_event_sequence = (uint32_t)esp_timer_get_time();
    memset(&s_model, 0, sizeof(s_model));
    memset(s_session_id, 0, sizeof(s_session_id));
    if (s_ui_queue) xQueueReset(s_ui_queue);

    s_scr = ui_pixel_screen_create("IELTS");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 56, 220, 184, UI_PAPER);

    s_progress = ui_pixel_label(panel, "-- / --", &lv_font_montserrat_14, UI_SKY_DARK);
    lv_obj_align(s_progress, LV_ALIGN_TOP_LEFT, 0, 0);
    s_audio = ui_pixel_label(panel, "AUDIO OFF", &lv_font_montserrat_14, UI_SKY_DARK);
    lv_obj_align(s_audio, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_word = ui_pixel_label(panel, "CONNECT", &lv_font_montserrat_28, UI_INK);
    lv_obj_set_width(s_word, 194);
    lv_obj_set_style_text_align(s_word, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_word, LV_ALIGN_TOP_MID, 0, 31);

    s_phonetic = ui_pixel_label(panel, "", &lv_font_montserrat_14, UI_SKY_DARK);
    lv_obj_set_width(s_phonetic, 194);
    lv_obj_set_style_text_align(s_phonetic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_phonetic, LV_ALIGN_TOP_MID, 0, 70);

    s_meaning = ui_pixel_label(panel, "正在连接手机热点…",
                               &lv_font_source_han_sans_sc_14_gb2312, UI_INK);
    lv_obj_set_width(s_meaning, 194);
    lv_obj_set_style_text_align(s_meaning, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_meaning, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_meaning, LV_ALIGN_TOP_MID, 0, 99);

    s_status = ui_pixel_label(s_scr, "正在启动…",
                              &lv_font_source_han_sans_sc_14_gb2312, UI_INK);
    lv_obj_set_width(s_status, 190);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 247);

    s_battery = ui_pixel_label(s_scr, "--", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_battery, 162, 35);
    s_mascot = ui_pixel_mascot_create(s_scr, 101, 271);
    s_timer = lv_timer_create(tick, 100, NULL);
    lv_screen_load(s_scr);

    if (!s_worker_task || !s_command_queue) {
        lv_label_set_text(s_status, "启动失败：内存不足");
        return;
    }
    vocabulary_command_t command = { .type = VOCAB_CMD_START };
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        lv_label_set_text(s_status, "启动队列忙，请返回重试");
    }
}

void demo_vocabulary_exit(void)
{
    s_page_active = false;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_word = s_phonetic = s_meaning = s_progress = NULL;
    s_status = s_audio = s_battery = s_mascot = NULL;
}

bool demo_vocabulary_can_exit(void)
{
    if (s_worker_busy) {
        if (s_status) lv_label_set_text(s_status, "正在同步，请稍候…");
        return false;
    }
    if (s_client_open && !s_finish_requested) {
        vocabulary_command_t command = { .type = VOCAB_CMD_FINISH };
        strlcpy(command.session_id, s_session_id, sizeof(command.session_id));
        if (xQueueSend(s_command_queue, &command, 0) == pdTRUE) {
            s_finish_requested = true;
            if (s_status) lv_label_set_text(s_status, "正在结束会话…");
        }
        return false;
    }
    return !s_client_open;
}

void demo_vocabulary_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!vocabulary_model_current(&s_model)) return;
    vocabulary_action_t action = { 0 };
    if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        action = vocabulary_model_up(&s_model);
        if (action.boundary) lv_label_set_text(s_status, "已经是本次第一个单词");
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        action = vocabulary_model_down(&s_model);
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        action = vocabulary_model_again(&s_model);
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        action = vocabulary_model_toggle_audio(&s_model);
        vocabulary_command_t command = {
            .type = VOCAB_CMD_AUDIO_SETTING,
            .enabled = s_model.auto_pronunciation,
        };
        xQueueSend(s_command_queue, &command, 0);
        lv_label_set_text(s_status, s_model.auto_pronunciation
                                      ? "自动读音已开启（音频将在下一阶段接入）"
                                      : "自动读音已关闭");
    } else {
        return;
    }
    queue_result(&action);
    if (action.changed || action.session_complete) refresh_card();
    if (action.session_complete) lv_label_set_text(s_status, "本次卡片已完成");
    if (action.changed && s_mascot) ui_pixel_mascot_jump(s_mascot);
}
