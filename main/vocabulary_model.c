#include "vocabulary_model.h"

#include <string.h>

static vocabulary_action_t base_action(const vocabulary_model_t *model)
{
    vocabulary_action_t action = { 0 };
    action.card_index = model->index;
    return action;
}
static void advance(vocabulary_model_t *model, vocabulary_action_t *action)
{
    if (model->index + 1 < model->count) {
        model->index++;
        model->meaning_visible = false;
        action->changed = true;
        action->play_pronunciation = model->auto_pronunciation;
    } else {
        action->session_complete = true;
    }
}

void vocabulary_model_init(vocabulary_model_t *model,
                           const vocabulary_card_t *cards, size_t count,
                           bool auto_pronunciation)
{
    memset(model, 0, sizeof(*model));
    if (count > VOCABULARY_MAX_CARDS) count = VOCABULARY_MAX_CARDS;
    if (cards && count > 0) {
        memcpy(model->cards, cards, count * sizeof(cards[0]));
        model->count = count;
    }
    model->auto_pronunciation = auto_pronunciation;
}

const vocabulary_card_t *vocabulary_model_current(const vocabulary_model_t *model)
{
    if (!model || model->count == 0 || model->index >= model->count) return NULL;
    return &model->cards[model->index];
}

vocabulary_action_t vocabulary_model_up(vocabulary_model_t *model)
{
    vocabulary_action_t action = base_action(model);
    if (model->index == 0) {
        action.boundary = true;
        return action;
    }
    model->index--;
    model->meaning_visible = false;
    action.changed = true;
    action.card_index = model->index;
    action.play_pronunciation = model->auto_pronunciation;
    return action;
}

vocabulary_action_t vocabulary_model_down(vocabulary_model_t *model)
{
    vocabulary_action_t action = base_action(model);
    if (!vocabulary_model_current(model)) return action;
    if (!model->meaning_visible) {
        model->meaning_visible = true;
        action.changed = true;
        return action;
    }

    vocabulary_card_t *card = &model->cards[model->index];
    if (card->grade == VOCABULARY_GRADE_NONE) {
        card->grade = VOCABULARY_GRADE_KNOWN;
        action.submit = true;
        action.grade = VOCABULARY_GRADE_KNOWN;
    }
    advance(model, &action);
    return action;
}

vocabulary_action_t vocabulary_model_again(vocabulary_model_t *model)
{
    vocabulary_action_t action = base_action(model);
    vocabulary_card_t *card = model->count > 0 && model->index < model->count
                                  ? &model->cards[model->index]
                                  : NULL;
    if (!card) return action;
    if (card->grade != VOCABULARY_GRADE_AGAIN) {
        card->grade = VOCABULARY_GRADE_AGAIN;
        action.submit = true;
        action.grade = VOCABULARY_GRADE_AGAIN;
    }
    advance(model, &action);
    return action;
}

vocabulary_action_t vocabulary_model_toggle_audio(vocabulary_model_t *model)
{
    vocabulary_action_t action = base_action(model);
    model->auto_pronunciation = !model->auto_pronunciation;
    action.changed = true;
    action.play_pronunciation = model->auto_pronunciation;
    return action;
}
