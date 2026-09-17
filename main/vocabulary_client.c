#include "vocabulary_client.h"

#include "demo_radio.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOCAB_NVS_NAMESPACE "vocab_cfg"
#define VOCAB_BASE_URL_BYTES 193
#define VOCAB_PAIRING_BYTES 65
#define VOCAB_TOKEN_BYTES 129
#define VOCAB_HTTP_RESPONSE_BYTES 4096
#define VOCAB_HTTP_TIMEOUT_MS 15000
#define VOCAB_WIFI_TIMEOUT_MS 20000
#define VOCAB_WIFI_CONNECTED BIT0
#define VOCAB_WIFI_FAILED BIT1

typedef struct {
    char ssid[33];
    char password[65];
    char base_url[VOCAB_BASE_URL_BYTES];
    char pairing_code[VOCAB_PAIRING_BYTES];
    char device_token[VOCAB_TOKEN_BYTES];
} vocabulary_config_t;

static vocabulary_config_t s_config;
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

static bool json_string_copy(cJSON *root, const char *name, char *out,
                             size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t length = strlen(item->valuestring);
    if (length >= out_size) return false;
    memcpy(out, item->valuestring, length + 1);
    return true;
}

static bool config_valid(const vocabulary_config_t *config)
{
    size_t url_length = strlen(config->base_url);
    return config->ssid[0] && config->pairing_code[0] &&
           url_length > 8 && url_length < VOCAB_BASE_URL_BYTES &&
           strncmp(config->base_url, "https://", 8) == 0;
}

static esp_err_t config_load(vocabulary_config_t *config)
{
    memset(config, 0, sizeof(*config));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOCAB_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t ssid_size = sizeof(config->ssid);
    size_t password_size = sizeof(config->password);
    size_t url_size = sizeof(config->base_url);
    size_t pairing_size = sizeof(config->pairing_code);
    size_t token_size = sizeof(config->device_token);
    err = nvs_get_str(handle, "ssid", config->ssid, &ssid_size);
    if (err == ESP_OK) err = nvs_get_str(handle, "password", config->password,
                                          &password_size);
    if (err == ESP_OK) err = nvs_get_str(handle, "base_url", config->base_url,
                                          &url_size);
    if (err == ESP_OK) err = nvs_get_str(handle, "pair_code", config->pairing_code,
                                          &pairing_size);
    esp_err_t token_err = nvs_get_str(handle, "token", config->device_token,
                                      &token_size);
    if (token_err == ESP_ERR_NVS_NOT_FOUND) config->device_token[0] = '\0';
    else if (err == ESP_OK && token_err != ESP_OK) err = token_err;
    nvs_close(handle);
    if (err != ESP_OK || !config_valid(config)) {
        secure_clear(config, sizeof(*config));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    return ESP_OK;
}

static esp_err_t config_store(const vocabulary_config_t *config)
{
    if (!config_valid(config)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOCAB_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "ssid", config->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", config->password);
    if (err == ESP_OK) err = nvs_set_str(handle, "base_url", config->base_url);
    if (err == ESP_OK) err = nvs_set_str(handle, "pair_code", config->pairing_code);
    if (err == ESP_OK) err = nvs_erase_key(handle, "token");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t token_store(const char *token)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOCAB_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "token", token);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t vocabulary_client_store_config_frame(const char *encoded)
{
    if (!encoded) return ESP_ERR_INVALID_ARG;
    uint8_t decoded[512];
    size_t decoded_length = 0;
    vocabulary_config_t config = { 0 };
    esp_err_t err = ESP_ERR_INVALID_ARG;
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_length,
                                   (const uint8_t *)encoded, strlen(encoded));
    if (rc != 0 || decoded_length == 0 || decoded_length >= sizeof(decoded)) goto done;
    decoded[decoded_length] = '\0';
    cJSON *root = cJSON_ParseWithLength((const char *)decoded, decoded_length);
    if (!root) goto done;
    bool valid = json_string_copy(root, "ssid", config.ssid, sizeof(config.ssid)) &&
                 json_string_copy(root, "password", config.password,
                                  sizeof(config.password)) &&
                 json_string_copy(root, "base_url", config.base_url,
                                  sizeof(config.base_url)) &&
                 json_string_copy(root, "pairing_code", config.pairing_code,
                                  sizeof(config.pairing_code));
    cJSON_Delete(root);
    if (!valid) goto done;
    size_t url_length = strlen(config.base_url);
    while (url_length > 8 && config.base_url[url_length - 1] == '/') {
        config.base_url[--url_length] = '\0';
    }
    err = config_store(&config);

done:
    secure_clear(&config, sizeof(config));
    secure_clear(decoded, sizeof(decoded));
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
        if (s_wifi_retries++ < 5) esp_wifi_connect();
        else if (s_wifi_events) xEventGroupSetBits(s_wifi_events, VOCAB_WIFI_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, VOCAB_WIFI_CONNECTED);
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
    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, VOCAB_WIFI_CONNECTED | VOCAB_WIFI_FAILED);
    }
    s_wifi_stopping = false;
}

static esp_err_t wifi_connect(const vocabulary_config_t *config)
{
    esp_err_t err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;
    xEventGroupClearBits(s_wifi_events, VOCAB_WIFI_CONNECTED | VOCAB_WIFI_FAILED);
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
    strlcpy((char *)wifi.sta.ssid, config->ssid, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, config->password, sizeof(wifi.sta.password));
    wifi.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi.sta.threshold.authmode = config->password[0] ? WIFI_AUTH_WPA2_PSK
                                                      : WIFI_AUTH_OPEN;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;
    s_wifi_retries = 0;

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, VOCAB_WIFI_CONNECTED | VOCAB_WIFI_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(VOCAB_WIFI_TIMEOUT_MS));
    if (bits & VOCAB_WIFI_CONNECTED) return ESP_OK;
    err = bits & VOCAB_WIFI_FAILED ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;

fail:
    wifi_stop();
    return err;
}

static esp_err_t http_write_all(esp_http_client_handle_t client,
                                const char *data, size_t length)
{
    while (length > 0) {
        int written = esp_http_client_write(client, data, (int)length);
        if (written <= 0) return ESP_FAIL;
        data += written;
        length -= (size_t)written;
    }
    return ESP_OK;
}

static esp_err_t http_json(esp_http_client_method_t method, const char *url,
                           const char *body, const char *token,
                           char *response, size_t response_size, int *status_code)
{
    if (!url || !response || response_size < 2) return ESP_ERR_INVALID_ARG;
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = VOCAB_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    char authorization[160] = { 0 };
    if (token && token[0]) {
        int length = snprintf(authorization, sizeof(authorization), "Bearer %s", token);
        if (length <= 0 || length >= (int)sizeof(authorization)) {
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    size_t body_length = body ? strlen(body) : 0;
    if (body) esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, (int)body_length);
    secure_clear(authorization, sizeof(authorization));
    if (err == ESP_OK && body_length > 0) err = http_write_all(client, body, body_length);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err != ESP_OK) goto done;

    *status_code = esp_http_client_get_status_code(client);
    size_t used = 0;
    while (used < response_size - 1) {
        int count = esp_http_client_read(client, response + used,
                                         response_size - 1 - used);
        if (count < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    response[used] = '\0';
    if (used == response_size - 1) {
        char extra;
        if (esp_http_client_read(client, &extra, 1) > 0) err = ESP_ERR_INVALID_SIZE;
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t build_url(char *out, size_t out_size, const char *path)
{
    int length = snprintf(out, out_size, "%s%s", s_config.base_url, path);
    return length > 0 && length < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t activate(void)
{
    char url[320];
    if (build_url(url, sizeof(url), "/v1/devices/activate") != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "pairing_code", s_config.pairing_code);
    cJSON_AddStringToObject(root, "device_name", "AI Passport");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    char response[512];
    int status = 0;
    esp_err_t err = http_json(HTTP_METHOD_POST, url, body, NULL,
                              response, sizeof(response), &status);
    secure_clear(body, strlen(body));
    free(body);
    if (err != ESP_OK) return err;
    if (status != 200) return ESP_ERR_INVALID_RESPONSE;
    root = cJSON_Parse(response);
    if (!root || !json_string_copy(root, "device_token", s_config.device_token,
                                   sizeof(s_config.device_token))) {
        if (root) cJSON_Delete(root);
        secure_clear(response, sizeof(response));
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(root);
    secure_clear(response, sizeof(response));
    err = token_store(s_config.device_token);
    if (err != ESP_OK) secure_clear(s_config.device_token, sizeof(s_config.device_token));
    return err;
}

static esp_err_t parse_session(const char *json, vocabulary_session_t *session)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    bool valid = json_string_copy(root, "session_id", session->session_id,
                                  sizeof(session->session_id));
    cJSON *complete = cJSON_GetObjectItemCaseSensitive(root, "complete");
    cJSON *cards = cJSON_GetObjectItemCaseSensitive(root, "cards");
    session->daily_complete = cJSON_IsTrue(complete);
    if (!valid || !cJSON_IsArray(cards) || cJSON_GetArraySize(cards) > VOCABULARY_MAX_CARDS) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cards) {
        vocabulary_card_t *card = &session->cards[session->card_count];
        bool card_valid = json_string_copy(item, "id", card->id, sizeof(card->id)) &&
                          json_string_copy(item, "word", card->word, sizeof(card->word)) &&
                          json_string_copy(item, "meaning", card->meaning,
                                           sizeof(card->meaning)) &&
                          json_string_copy(item, "phonetic", card->phonetic,
                                           sizeof(card->phonetic));
        if (!card_valid) {
            cJSON_Delete(root);
            memset(session, 0, sizeof(*session));
            return ESP_ERR_INVALID_RESPONSE;
        }
        session->card_count++;
    }
    cJSON_Delete(root);
    return session->card_count > 0 || session->daily_complete
               ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t start_session(vocabulary_session_t *session)
{
    char url[320];
    if (build_url(url, sizeof(url), "/v1/study/sessions") != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    char response[VOCAB_HTTP_RESPONSE_BYTES];
    int status = 0;
    esp_err_t err = http_json(HTTP_METHOD_POST, url, "{\"batch_size\":10}",
                              s_config.device_token, response, sizeof(response), &status);
    if (err == ESP_OK && status == 200) err = parse_session(response, session);
    else if (err == ESP_OK) err = status == 401 ? ESP_ERR_INVALID_STATE
                                                 : ESP_ERR_INVALID_RESPONSE;
    secure_clear(response, sizeof(response));
    return err;
}

esp_err_t vocabulary_client_open(vocabulary_session_t *session)
{
    if (!session) return ESP_ERR_INVALID_ARG;
    memset(session, 0, sizeof(*session));
    secure_clear(&s_config, sizeof(s_config));
    esp_err_t err = config_load(&s_config);
    if (err != ESP_OK) return err;
    err = wifi_connect(&s_config);
    if (err != ESP_OK) goto fail;
    if (!s_config.device_token[0]) {
        err = activate();
        if (err != ESP_OK) goto fail;
    }
    err = start_session(session);
    if (err == ESP_ERR_INVALID_STATE) {
        s_config.device_token[0] = '\0';
        err = activate();
        if (err == ESP_OK) err = start_session(session);
    }
    if (err == ESP_OK) return ESP_OK;

fail:
    vocabulary_client_close();
    return err;
}

esp_err_t vocabulary_client_submit(const char *session_id, const char *card_id,
                                   vocabulary_grade_t grade,
                                   const char *idempotency_key)
{
    if (!session_id || !card_id || !idempotency_key ||
        (grade != VOCABULARY_GRADE_KNOWN && grade != VOCABULARY_GRADE_AGAIN)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[180];
    int length = snprintf(path, sizeof(path),
                          "/v1/study/sessions/%s/cards/%s/result",
                          session_id, card_id);
    char url[400];
    if (length <= 0 || length >= (int)sizeof(path) ||
        build_url(url, sizeof(url), path) != ESP_OK) return ESP_ERR_INVALID_SIZE;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "result",
                            grade == VOCABULARY_GRADE_KNOWN ? "known" : "again");
    cJSON_AddStringToObject(root, "idempotency_key", idempotency_key);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    char response[384];
    int status = 0;
    esp_err_t err = http_json(HTTP_METHOD_PUT, url, body, s_config.device_token,
                              response, sizeof(response), &status);
    secure_clear(body, strlen(body));
    free(body);
    secure_clear(response, sizeof(response));
    if (err == ESP_OK && status != 200) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}

esp_err_t vocabulary_client_finish(const char *session_id)
{
    if (!session_id || !session_id[0]) return ESP_ERR_INVALID_ARG;
    char path[128];
    int length = snprintf(path, sizeof(path), "/v1/study/sessions/%s/finish", session_id);
    char url[360];
    if (length <= 0 || length >= (int)sizeof(path) ||
        build_url(url, sizeof(url), path) != ESP_OK) return ESP_ERR_INVALID_SIZE;
    char response[384];
    int status = 0;
    esp_err_t err = http_json(HTTP_METHOD_POST, url, "{}", s_config.device_token,
                              response, sizeof(response), &status);
    secure_clear(response, sizeof(response));
    if (err == ESP_OK && status != 200) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}

void vocabulary_client_close(void)
{
    wifi_stop();
    secure_clear(&s_config, sizeof(s_config));
}
