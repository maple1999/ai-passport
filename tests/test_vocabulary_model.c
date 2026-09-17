#include "vocabulary_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static vocabulary_card_t card(const char *id, const char *word)
{
    vocabulary_card_t value = { 0 };
    snprintf(value.id, sizeof(value.id), "%s", id);
    snprintf(value.word, sizeof(value.word), "%s", word);
    snprintf(value.meaning, sizeof(value.meaning), "meaning-%s", id);
    return value;
}

int main(void)
{
    vocabulary_card_t cards[] = { card("c1", "adapt"), card("c2", "derive") };
    vocabulary_model_t model;
    vocabulary_model_init(&model, cards, 2, false);

    assert(strcmp(vocabulary_model_current(&model)->word, "adapt") == 0);
    vocabulary_action_t action = vocabulary_model_up(&model);
    assert(action.boundary && model.index == 0);

    action = vocabulary_model_down(&model);
    assert(action.changed && !action.submit && model.meaning_visible);
    action = vocabulary_model_down(&model);
    assert(action.submit && action.grade == VOCABULARY_GRADE_KNOWN);
    assert(model.index == 1 && !model.meaning_visible);

    action = vocabulary_model_up(&model);
    assert(action.changed && model.index == 0);
    action = vocabulary_model_down(&model);
    assert(model.meaning_visible);
    action = vocabulary_model_down(&model);
    assert(!action.submit && model.index == 1);

    action = vocabulary_model_again(&model);
    assert(action.submit && action.grade == VOCABULARY_GRADE_AGAIN);
    assert(action.session_complete);
    action = vocabulary_model_again(&model);
    assert(!action.submit && action.session_complete);

    action = vocabulary_model_up(&model);
    assert(model.index == 0);
    action = vocabulary_model_again(&model);
    assert(action.submit && action.grade == VOCABULARY_GRADE_AGAIN);
    assert(model.index == 1);

    action = vocabulary_model_toggle_audio(&model);
    assert(model.auto_pronunciation && action.play_pronunciation);
    action = vocabulary_model_toggle_audio(&model);
    assert(!model.auto_pronunciation && !action.play_pronunciation);

    puts("vocabulary_model tests: PASS");
    return 0;
}
