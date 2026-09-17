#pragma once

#include "esp_err.h"
#include "vocabulary_model.h"

#include <stddef.h>

#define VOCABULARY_SESSION_ID_BYTES 40

typedef struct {
    char session_id[VOCABULARY_SESSION_ID_BYTES];
    vocabulary_card_t cards[VOCABULARY_MAX_CARDS];
    size_t card_count;
    bool daily_complete;
} vocabulary_session_t;

// Decode and persist one base64 JSON payload received after VOCAB_CONFIG_V1.
// The payload contains ssid, password, base_url, and pairing_code. A new
// provisioning frame invalidates the old device token.
esp_err_t vocabulary_client_store_config_frame(const char *encoded);

// Connect to the configured phone hotspot, activate when needed, and create a
// bounded study session. The client owns Wi-Fi until vocabulary_client_close.
esp_err_t vocabulary_client_open(vocabulary_session_t *session);
esp_err_t vocabulary_client_submit(const char *session_id, const char *card_id,
                                   vocabulary_grade_t grade,
                                   const char *idempotency_key);
esp_err_t vocabulary_client_finish(const char *session_id);
void vocabulary_client_close(void);
