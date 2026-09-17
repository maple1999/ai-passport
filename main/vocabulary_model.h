#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VOCABULARY_MAX_CARDS 10
#define VOCABULARY_WORD_BYTES 48
#define VOCABULARY_MEANING_BYTES 160
#define VOCABULARY_PHONETIC_BYTES 64
#define VOCABULARY_ID_BYTES 40

typedef enum {
    VOCABULARY_GRADE_NONE = 0,
    VOCABULARY_GRADE_KNOWN,
    VOCABULARY_GRADE_AGAIN,
} vocabulary_grade_t;

typedef struct {
    char id[VOCABULARY_ID_BYTES];
    char word[VOCABULARY_WORD_BYTES];
    char meaning[VOCABULARY_MEANING_BYTES];
    char phonetic[VOCABULARY_PHONETIC_BYTES];
    vocabulary_grade_t grade;
} vocabulary_card_t;

typedef struct {
    vocabulary_card_t cards[VOCABULARY_MAX_CARDS];
    size_t count;
    size_t index;
    bool meaning_visible;
    bool auto_pronunciation;
} vocabulary_model_t;

typedef struct {
    bool changed;
    bool boundary;
    bool session_complete;
    bool submit;
    bool play_pronunciation;
    vocabulary_grade_t grade;
    size_t card_index;
} vocabulary_action_t;

void vocabulary_model_init(vocabulary_model_t *model,
                           const vocabulary_card_t *cards, size_t count,
                           bool auto_pronunciation);
const vocabulary_card_t *vocabulary_model_current(const vocabulary_model_t *model);
vocabulary_action_t vocabulary_model_up(vocabulary_model_t *model);
vocabulary_action_t vocabulary_model_down(vocabulary_model_t *model);
vocabulary_action_t vocabulary_model_again(vocabulary_model_t *model);
vocabulary_action_t vocabulary_model_toggle_audio(vocabulary_model_t *model);
