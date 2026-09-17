// SiliconFlow speech recognition page and local USB provisioning service.
//
// The device records 16 kHz/16-bit/mono PCM into the dedicated `asrbuf`
// partition, then streams a WAV multipart body to SiliconFlow. Audio never
// needs to coexist as one large RAM allocation with Wi-Fi and TLS.
#include "demo.h"
#include "demo_radio.h"
#include "asr_audio.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "font_source_han_sans_sc_14_gb2312.h"
#include "ui_pixel.h"

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_asr";

#define ASR_MODEL "XingChenAGI/XingChenASR-V3.2-Ultra"
#define ASR_URL "https://api.siliconflow.cn/v1/audio/transcriptions"
#define ASR_PARTITION_LABEL "asrbuf"
#define ASR_MAX_RECORD_SECONDS 8u
#define ASR_PCM_BYTES_PER_SECOND \
    (ASR_SAMPLE_RATE_HZ * ASR_SAMPLE_CHANNELS * (ASR_SAMPLE_BITS / 8u))
#define ASR_MAX_PCM_BYTES (ASR_MAX_RECORD_SECONDS * ASR_PCM_BYTES_PER_SECOND)
#define ASR_AUDIO_CHUNK_BYTES 1024u
#define ASR_RESULT_BYTES 1152u
#define ASR_STATUS_BYTES 128u
#define ASR_WORKER_STACK 10240u
#define ASR_CONFIG_TASK_STACK 5120u
#define ASR_CONFIG_LINE_BYTES 768u
#define ASR_WIFI_TIMEOUT_MS 20000u
#define ASR_HTTP_TIMEOUT_MS 30000u
#define ASR_BOUNDARY "----FoloToyPassportASR7MA4YWxk"

#define ASR_WIFI_CONNECTED BIT0
#define ASR_WIFI_FAILED BIT1

typedef enum {
    ASR_STATE_CONFIG_REQUIRED = 0,
    ASR_STATE_READY,
    ASR_STATE_RECORDING,
    ASR_STATE_CONNECTING,
    ASR_STATE_UPLOADING,
    ASR_STATE_RESULT,
    ASR_STATE_ERROR,
} asr_state_t;

typedef enum {
    ASR_CMD_RUN = 1,
} asr_command_t;

typedef struct {
    char ssid[33];
    char password[65];
    char api_key[193];
} asr_config_t;

typedef struct {
    asr_state_t state;
    char status[ASR_STATUS_BYTES];
    char result[ASR_RESULT_BYTES];
} asr_ui_message_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_status;
static lv_obj_t *s_result_panel;
static lv_obj_t *s_result;
static lv_obj_t *s_hint;
static lv_obj_t *s_battery;
static lv_timer_t *s_timer;
static QueueHandle_t s_command_queue;
static QueueHandle_t s_ui_queue;
static TaskHandle_t s_worker_task;
static TaskHandle_t s_config_task;
static volatile asr_state_t s_state = ASR_STATE_CONFIG_REQUIRED;
static volatile bool s_stop_recording;
static volatile uint32_t s_config_generation;
static uint32_t s_seen_config_generation;
static bool s_page_active;

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_wifi_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_event_registered;
static bool s_ip_event_registered;
static bool s_wifi_stopping;
static int s_wifi_retries;

static void secure_clear(void *data, size_t size)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (size--) *p++ = 0;
}

static bool config_valid(const asr_config_t *cfg)
{
    return cfg->ssid[0] != '\0' && cfg->api_key[0] != '\0';
}

static esp_err_t config_load(asr_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("asr_cfg", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t ssid_len = sizeof(cfg->ssid);
    size_t password_len = sizeof(cfg->password);
    size_t key_len = sizeof(cfg->api_key);
    err = nvs_get_str(handle, "ssid", cfg->ssid, &ssid_len);
    if (err == ESP_OK) err = nvs_get_str(handle, "password", cfg->password,
                                          &password_len);
    if (err == ESP_OK) err = nvs_get_str(handle, "api_key", cfg->api_key,
                                          &key_len);
    nvs_close(handle);
    if (err != ESP_OK || !config_valid(cfg)) {
        secure_clear(cfg, sizeof(*cfg));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    return ESP_OK;
}

static esp_err_t config_store(const asr_config_t *cfg)
{
    if (!config_valid(cfg)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("asr_cfg", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "ssid", cfg->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", cfg->password);
    if (err == ESP_OK) err = nvs_set_str(handle, "api_key", cfg->api_key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void ui_publish(asr_state_t state, const char *status, const char *result)
{
    asr_ui_message_t message = { .state = state };
    if (status) strlcpy(message.status, status, sizeof(message.status));
    if (result) strlcpy(message.result, result, sizeof(message.result));
    s_state = state;
    if (s_ui_queue) xQueueOverwrite(s_ui_queue, &message);
}

static void serial_write_line(const char *line)
{
    const uint8_t *p = (const uint8_t *)line;
    size_t remaining = strlen(line);
    while (remaining > 0) {
        size_t chunk = remaining > 256 ? 256 : remaining;
        int written = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(500));
        if (written <= 0) return;
        p += written;
        remaining -= (size_t)written;
    }
}

static bool json_string_copy(cJSON *root, const char *name, char *out,
                             size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t len = strlen(item->valuestring);
    if (len >= out_size) return false;
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static void handle_config_frame(char *encoded)
{
    uint8_t decoded[512];
    size_t decoded_len = 0;
    asr_config_t cfg = { 0 };
    esp_err_t err = ESP_FAIL;

    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                   (const uint8_t *)encoded, strlen(encoded));
    if (rc != 0 || decoded_len == 0 || decoded_len >= sizeof(decoded)) {
        serial_write_line("ASR_CONFIG_V1 ERROR invalid_base64\n");
        goto done;
    }
    decoded[decoded_len] = '\0';
    cJSON *root = cJSON_ParseWithLength((const char *)decoded, decoded_len);
    if (!root) {
        serial_write_line("ASR_CONFIG_V1 ERROR invalid_json\n");
        goto done;
    }
    bool valid = json_string_copy(root, "ssid", cfg.ssid, sizeof(cfg.ssid)) &&
                 json_string_copy(root, "password", cfg.password,
                                  sizeof(cfg.password)) &&
                 json_string_copy(root, "api_key", cfg.api_key,
                                  sizeof(cfg.api_key));
    cJSON_Delete(root);
    if (!valid || !config_valid(&cfg)) {
        serial_write_line("ASR_CONFIG_V1 ERROR invalid_fields\n");
        goto done;
    }

    err = config_store(&cfg);
    if (err == ESP_OK) {
        s_config_generation++;
        serial_write_line("ASR_CONFIG_V1 OK\n");
    } else {
        serial_write_line("ASR_CONFIG_V1 ERROR nvs_write\n");
    }

done:
    secure_clear(&cfg, sizeof(cfg));
    secure_clear(decoded, sizeof(decoded));
    secure_clear(encoded, strlen(encoded));
    if (err != ESP_OK) ESP_LOGW(TAG, "ASR 配置帧未保存: %s", esp_err_to_name(err));
}

static void config_task(void *arg)
{
    (void)arg;
    char line[ASR_CONFIG_LINE_BYTES];
    size_t used = 0;
    uint8_t input[64];
    const char prefix[] = "ASR_CONFIG_V1 ";

    for (;;) {
        int count = usb_serial_jtag_read_bytes(input, sizeof(input),
                                               pdMS_TO_TICKS(500));
        if (count <= 0) continue;
        for (int i = 0; i < count; i++) {
            char ch = (char)input[i];
            if (ch == '\r' || ch == '\n') {
                line[used] = '\0';
                if (used > sizeof(prefix) - 1 &&
                    memcmp(line, prefix, sizeof(prefix) - 1) == 0) {
                    handle_config_frame(line + sizeof(prefix) - 1);
                }
                secure_clear(line, used);
                used = 0;
            } else if (used < sizeof(line) - 1) {
                line[used++] = ch;
            } else {
                secure_clear(line, sizeof(line));
                used = 0;
                serial_write_line("ASR_CONFIG_V1 ERROR frame_too_large\n");
            }
        }
    }
}

void demo_asr_service_start(void)
{
    if (s_config_task) return;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .rx_buffer_size = 1024,
            .tx_buffer_size = 1024,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB 配置服务初始化失败: %s", esp_err_to_name(err));
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(config_task, "asr_config", ASR_CONFIG_TASK_STACK, NULL, 3,
                    &s_config_task) != pdPASS) {
        s_config_task = NULL;
        ESP_LOGE(TAG, "USB 配置任务创建失败");
        return;
    }
    ESP_LOGI(TAG, "ASR USB 配置服务就绪");
}

static esp_err_t record_to_flash(uint32_t *pcm_bytes)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, ASR_PARTITION_LABEL);
    if (!partition || partition->size < ASR_WAV_HEADER_BYTES + ASR_MAX_PCM_BYTES) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) return err;
    err = bsp_audio_set_format(ASR_SAMPLE_RATE_HZ, ASR_SAMPLE_BITS,
                               ASR_SAMPLE_CHANNELS);
    if (err != ESP_OK) return err;

    uint8_t chunk[ASR_AUDIO_CHUNK_BYTES];
    uint32_t written = 0;
    s_stop_recording = false;
    while (written < ASR_MAX_PCM_BYTES && !s_stop_recording) {
        size_t want = ASR_MAX_PCM_BYTES - written;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        err = bsp_audio_read(chunk, want);
        if (err != ESP_OK) break;
        err = esp_partition_write(partition, ASR_WAV_HEADER_BYTES + written,
                                  chunk, want);
        if (err != ESP_OK) break;
        written += (uint32_t)want;
    }
    secure_clear(chunk, sizeof(chunk));
    if (err != ESP_OK) return err;
    if (written < ASR_PCM_BYTES_PER_SECOND / 2u) return ESP_ERR_INVALID_SIZE;

    uint8_t header[ASR_WAV_HEADER_BYTES];
    asr_wav_build_header(header, written);
    err = esp_partition_write(partition, 0, header, sizeof(header));
    secure_clear(header, sizeof(header));
    if (err == ESP_OK) *pcm_bytes = written;
    return err;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_stopping) return;
        if (s_wifi_retries++ < 3) esp_wifi_connect();
        else if (s_wifi_events) xEventGroupSetBits(s_wifi_events, ASR_WIFI_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, ASR_WIFI_CONNECTED);
    }
}

static void wifi_stop(void)
{
    s_wifi_stopping = true;
    if (s_wifi_event_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_handler);
        s_wifi_event_registered = false;
    }
    if (s_ip_event_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_handler);
        s_ip_event_registered = false;
    }
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_wifi_netif) {
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
    }
    if (s_wifi_events) xEventGroupClearBits(s_wifi_events,
                                            ASR_WIFI_CONNECTED | ASR_WIFI_FAILED);
    s_wifi_stopping = false;
}

static esp_err_t wifi_connect(const asr_config_t *cfg)
{
    esp_err_t err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;
    xEventGroupClearBits(s_wifi_events, ASR_WIFI_CONNECTED | ASR_WIFI_FAILED);
    s_wifi_stopping = false;

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi_netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) goto fail;
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) goto fail;
    s_wifi_event_registered = true;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event, NULL, &s_ip_handler);
    if (err != ESP_OK) goto fail;
    s_ip_event_registered = true;

    wifi_config_t wifi = { 0 };
    strlcpy((char *)wifi.sta.ssid, cfg->ssid, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, cfg->password, sizeof(wifi.sta.password));
    wifi.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi.sta.threshold.authmode = cfg->password[0] ? WIFI_AUTH_WPA2_PSK
                                                   : WIFI_AUTH_OPEN;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;
    s_wifi_retries = 0;

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, ASR_WIFI_CONNECTED | ASR_WIFI_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(ASR_WIFI_TIMEOUT_MS));
    if (bits & ASR_WIFI_CONNECTED) return ESP_OK;
    err = (bits & ASR_WIFI_FAILED) ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;

fail:
    wifi_stop();
    return err;
}

static esp_err_t http_write_all(esp_http_client_handle_t client,
                                const void *data, size_t length)
{
    const char *p = (const char *)data;
    while (length > 0) {
        int written = esp_http_client_write(client, p, (int)length);
        if (written <= 0) return ESP_FAIL;
        p += written;
        length -= (size_t)written;
    }
    return ESP_OK;
}

static void utf8_copy(char *out, size_t out_size, const char *input)
{
    if (out_size == 0) return;
    size_t length = strlen(input);
    if (length >= out_size) length = out_size - 1;
    memcpy(out, input, length);
    out[length] = '\0';
    if (input[length] == '\0') return;

    size_t sequence_start = length;
    while (sequence_start > 0 &&
           ((uint8_t)out[sequence_start - 1] & 0xC0u) == 0x80u) {
        sequence_start--;
    }
    if (sequence_start == 0) {
        out[0] = '\0';
        return;
    }
    sequence_start--;
    uint8_t lead = (uint8_t)out[sequence_start];
    size_t expected = lead < 0x80u ? 1u :
                      (lead & 0xE0u) == 0xC0u ? 2u :
                      (lead & 0xF0u) == 0xE0u ? 3u :
                      (lead & 0xF8u) == 0xF0u ? 4u : 1u;
    if (length - sequence_start < expected) out[sequence_start] = '\0';
}

static esp_err_t parse_response(int status, const char *json, char *out,
                                size_t out_size)
{
    cJSON *root = cJSON_Parse(json);
    if (status == 200 && root) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            utf8_copy(out, out_size, text->valuestring);
            cJSON_Delete(root);
            return out[0] ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
    }

    const char *message_text = NULL;
    if (root) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(message)) message_text = message->valuestring;
    }
    if (message_text) snprintf(out, out_size, "HTTP %d: %s", status, message_text);
    else snprintf(out, out_size, "HTTP %d，服务未返回可识别文本", status);
    if (root) cJSON_Delete(root);
    return ESP_FAIL;
}

static esp_err_t upload_audio(const asr_config_t *cfg, uint32_t pcm_bytes,
                              char *result, size_t result_size)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, ASR_PARTITION_LABEL);
    if (!partition) return ESP_ERR_NOT_FOUND;

    char prefix[384];
    char suffix[64];
    int prefix_len = snprintf(
        prefix, sizeof(prefix),
        "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"passport.wav\"\r\nContent-Type: audio/wav\r\n\r\n",
        ASR_BOUNDARY, ASR_MODEL, ASR_BOUNDARY);
    int suffix_len = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n",
                              ASR_BOUNDARY);
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(prefix) ||
        suffix_len <= 0 || suffix_len >= (int)sizeof(suffix)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t wav_bytes = ASR_WAV_HEADER_BYTES + pcm_bytes;
    size_t content_length = asr_multipart_length((size_t)prefix_len, wav_bytes,
                                                 (size_t)suffix_len);
    if (content_length == SIZE_MAX || content_length > INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t http_config = {
        .url = ASR_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = ASR_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) return ESP_ERR_NO_MEM;

    char authorization[224];
    char content_type[96];
    snprintf(authorization, sizeof(authorization), "Bearer %s", cfg->api_key);
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s",
             ASR_BOUNDARY);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, (int)content_length);
    secure_clear(authorization, sizeof(authorization));
    if (err != ESP_OK) goto done;
    err = http_write_all(client, prefix, (size_t)prefix_len);

    uint8_t chunk[ASR_AUDIO_CHUNK_BYTES];
    for (size_t offset = 0; err == ESP_OK && offset < wav_bytes;) {
        size_t count = wav_bytes - offset;
        if (count > sizeof(chunk)) count = sizeof(chunk);
        err = esp_partition_read(partition, offset, chunk, count);
        if (err == ESP_OK) err = http_write_all(client, chunk, count);
        offset += count;
    }
    secure_clear(chunk, sizeof(chunk));
    if (err == ESP_OK) err = http_write_all(client, suffix, (size_t)suffix_len);
    if (err != ESP_OK) goto done;

    int headers = esp_http_client_fetch_headers(client);
    if (headers < 0) {
        err = ESP_FAIL;
        goto done;
    }
    int status = esp_http_client_get_status_code(client);
    char response[4096];
    size_t used = 0;
    while (used < sizeof(response) - 1) {
        int count = esp_http_client_read(client, response + used,
                                         sizeof(response) - 1 - used);
        if (count < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    response[used] = '\0';
    err = parse_response(status, response, result, result_size);
    secure_clear(response, sizeof(response));

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void worker_task(void *arg)
{
    (void)arg;
    asr_command_t command;
    for (;;) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command != ASR_CMD_RUN) continue;

        asr_config_t cfg;
        if (config_load(&cfg) != ESP_OK) {
            ui_publish(ASR_STATE_CONFIG_REQUIRED,
                       "请先通过 USB 写入 Wi-Fi 与 API Key", "");
            continue;
        }

        ui_publish(ASR_STATE_RECORDING, "正在录音；按 OK 可提前结束", "");
        uint32_t pcm_bytes = 0;
        esp_err_t err = record_to_flash(&pcm_bytes);
        if (err != ESP_OK) {
            char status[ASR_STATUS_BYTES];
            snprintf(status, sizeof(status), "录音失败：%s", esp_err_to_name(err));
            ui_publish(ASR_STATE_ERROR, status, "");
            secure_clear(&cfg, sizeof(cfg));
            continue;
        }

        ui_publish(ASR_STATE_CONNECTING, "正在连接 Wi-Fi…", "");
        err = wifi_connect(&cfg);
        if (err != ESP_OK) {
            char status[ASR_STATUS_BYTES];
            snprintf(status, sizeof(status), "Wi-Fi 失败：%s", esp_err_to_name(err));
            ui_publish(ASR_STATE_ERROR, status, "");
            secure_clear(&cfg, sizeof(cfg));
            continue;
        }

        ui_publish(ASR_STATE_UPLOADING, "正在上传并识别…", "");
        char result[ASR_RESULT_BYTES] = { 0 };
        err = upload_audio(&cfg, pcm_bytes, result, sizeof(result));
        wifi_stop();
        secure_clear(&cfg, sizeof(cfg));
        if (err == ESP_OK) {
            ui_publish(ASR_STATE_RESULT, "识别完成；OK 再录一次", result);
        } else {
            if (!result[0]) {
                snprintf(result, sizeof(result), "请求失败：%s",
                         esp_err_to_name(err));
            }
            ui_publish(ASR_STATE_ERROR, "识别失败；OK 重试", result);
        }
        secure_clear(result, sizeof(result));
    }
}

static void apply_ui_message(const asr_ui_message_t *message)
{
    if (!s_page_active || !s_status || !s_result) return;
    lv_label_set_text(s_status, message->status);
    lv_label_set_text(s_result, message->result[0] ? message->result :
                     (message->state == ASR_STATE_CONFIG_REQUIRED
                          ? "在电脑运行 tools/configure_asr.py\n配置手机热点和 SiliconFlow Key"
                          : "语音会在这里显示为文字"));
    switch (message->state) {
    case ASR_STATE_RECORDING:
        lv_label_set_text(s_hint, "OK STOP  |  MAX 8 SEC");
        break;
    case ASR_STATE_CONNECTING:
    case ASR_STATE_UPLOADING:
        lv_label_set_text(s_hint, "PLEASE WAIT");
        break;
    default:
        lv_label_set_text(s_hint, "OK RECORD  |  HOLD OK BACK");
        break;
    }
    lv_obj_scroll_to_y(s_result_panel, 0, LV_ANIM_OFF);
}

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    asr_ui_message_t message;
    if (s_ui_queue && xQueueReceive(s_ui_queue, &message, 0) == pdTRUE) {
        apply_ui_message(&message);
    }
    if (s_seen_config_generation != s_config_generation) {
        s_seen_config_generation = s_config_generation;
        asr_config_t cfg;
        if (config_load(&cfg) == ESP_OK) {
            secure_clear(&cfg, sizeof(cfg));
            if (s_state != ASR_STATE_RECORDING &&
                s_state != ASR_STATE_CONNECTING &&
                s_state != ASR_STATE_UPLOADING) {
                ui_publish(ASR_STATE_READY, "配置已保存，按 OK 开始录音", "");
            }
        }
    }

    static uint8_t battery_divider;
    if (++battery_divider >= 20) {
        battery_divider = 0;
        int soc = bsp_battery_soc();
        if (s_battery) {
            if (soc >= 0) lv_label_set_text_fmt(s_battery, "%d%%", soc);
            else lv_label_set_text(s_battery, "--%");
        }
    }
}

void demo_asr_enter(void)
{
    if (!s_command_queue) s_command_queue = xQueueCreate(1, sizeof(asr_command_t));
    if (!s_ui_queue) s_ui_queue = xQueueCreate(1, sizeof(asr_ui_message_t));
    if (!s_worker_task && s_command_queue && s_ui_queue) {
        if (xTaskCreate(worker_task, "voice_asr", ASR_WORKER_STACK, NULL, 4,
                        &s_worker_task) != pdPASS) {
            s_worker_task = NULL;
        }
    }

    s_scr = ui_pixel_screen_create("VOICE ASR");
    s_battery = lv_label_create(s_scr);
    lv_obj_set_pos(s_battery, 160, 20);
    lv_obj_set_width(s_battery, 34);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(UI_INK), 0);
    lv_label_set_text(s_battery, "--%");

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 52, 216, 190, UI_PAPER);
    s_status = lv_label_create(panel);
    lv_obj_set_width(s_status, 190);
    lv_obj_set_style_text_font(s_status,
                               &lv_font_source_han_sans_sc_14_gb2312, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 2, 0);

    s_result_panel = lv_obj_create(panel);
    lv_obj_set_pos(s_result_panel, 0, 42);
    lv_obj_set_size(s_result_panel, 192, 104);
    lv_obj_set_style_bg_color(s_result_panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(s_result_panel, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_result_panel, 2, 0);
    lv_obj_set_style_pad_all(s_result_panel, 6, 0);
    lv_obj_set_scroll_dir(s_result_panel, LV_DIR_VER);

    s_result = lv_label_create(s_result_panel);
    lv_obj_set_width(s_result, 176);
    lv_obj_set_style_text_font(s_result,
                               &lv_font_source_han_sans_sc_14_gb2312, 0);
    lv_obj_set_style_text_color(s_result, lv_color_hex(UI_INK), 0);
    lv_label_set_long_mode(s_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_result, LV_ALIGN_TOP_LEFT, 0, 0);

    s_hint = lv_label_create(panel);
    lv_obj_set_width(s_hint, 190);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, 0);

    ui_pixel_mascot_create(s_scr, 101, 244);
    s_page_active = true;
    s_seen_config_generation = s_config_generation;

    asr_config_t cfg;
    if (config_load(&cfg) == ESP_OK) {
        secure_clear(&cfg, sizeof(cfg));
        ui_publish(ASR_STATE_READY, "就绪，按 OK 开始录音", "");
    } else {
        ui_publish(ASR_STATE_CONFIG_REQUIRED,
                   "需要配置 Wi-Fi 与 SiliconFlow Key", "");
    }
    s_timer = lv_timer_create(ui_tick, 100, NULL);
    lv_screen_load(s_scr);
}

void demo_asr_exit(void)
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
    s_status = s_result_panel = s_result = s_hint = s_battery = NULL;
}

bool demo_asr_can_exit(void)
{
    return s_state != ASR_STATE_RECORDING && s_state != ASR_STATE_CONNECTING &&
           s_state != ASR_STATE_UPLOADING;
}

void demo_asr_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG && !demo_asr_can_exit()) {
        if (s_state == ASR_STATE_RECORDING) s_stop_recording = true;
        if (s_status) lv_label_set_text(s_status, "正在结束当前操作，请稍候…");
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        if (s_state == ASR_STATE_RECORDING) {
            s_stop_recording = true;
            return;
        }
        if (s_state == ASR_STATE_READY || s_state == ASR_STATE_RESULT ||
            s_state == ASR_STATE_ERROR) {
            asr_command_t command = ASR_CMD_RUN;
            if (s_command_queue) xQueueOverwrite(s_command_queue, &command);
        } else if (s_state == ASR_STATE_CONFIG_REQUIRED && s_status) {
            lv_label_set_text(s_status, "等待 USB 配置；保存后会自动刷新");
        }
        return;
    }
    if ((btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) && ev == BSP_BTN_CLICK &&
        s_result_panel) {
        int delta = btn == BSP_BTN_UP ? 38 : -38;
        lv_obj_scroll_by(s_result_panel, 0, delta, LV_ANIM_ON);
    }
}
