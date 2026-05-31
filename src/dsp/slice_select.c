#include "slice_select.h"

float slice_select_weight_at(int slice_idx, float anchor) {
    static const float locked[8] = {0.0f, 0.5f, 1.0f, 0.7f, 0.0f, 0.5f, 1.0f, 1.2f};
    if (slice_idx < 0) slice_idx = 0;
    if (slice_idx > 7) slice_idx = 7;
    if (anchor < 0.0f) anchor = 0.0f;
    if (anchor > 1.0f) anchor = 1.0f;
    return 1.0f * (1.0f - anchor) + locked[slice_idx] * anchor;
}

void slice_select_apply_phrase(const slice_inputs_t *in,
                                float *out_complexity,
                                float *out_anchor,
                                float *out_roll) {
    *out_complexity = in->complexity;
    *out_anchor = in->anchor;
    *out_roll = in->roll;

    if (in->phrase_bars <= 0) return;
    int is_fill = (in->bar_in_phrase == in->phrase_bars - 1);
    if (!is_fill) return;

    float f = in->fill;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;

    *out_complexity = in->complexity + (1.0f - in->complexity) * f;
    *out_anchor     = in->anchor * (1.0f - f);
    *out_roll       = in->roll   * (1.0f - f);
}

int slice_select_next(const slice_inputs_t *in, slice_rand_fn rand_fn, void *rand_ctx) {
    float eff_complexity, eff_anchor, eff_roll;
    slice_select_apply_phrase(in, &eff_complexity, &eff_anchor, &eff_roll);

    float r = rand_fn(rand_ctx);
    if (r < (1.0f - eff_roll)) {
        /* MOVE branch */
        float p_swap = eff_complexity * slice_select_weight_at(in->beat_position, eff_anchor);
        if (rand_fn(rand_ctx) < p_swap) {
            int j = (int)(rand_fn(rand_ctx) * 8.0f);
            if (j < 0) j = 0;
            if (j > 7) j = 7;
            return j;
        }
        /* No swap: play the natural slice for this beat.
         * Using beat_position rather than (current_slice+1) ensures that a
         * random swap only affects the one beat it lands on. With current_slice+1,
         * a swap at beat 5 → slice 2 would cause beats 6,7,0,1... to play
         * slices 3,4,5,6... — permanent drift until another swap corrected it.
         * Anchor still modulates the swap probability via the weight function. */
        return in->beat_position;
    }
    /* STAY branch */
    if (rand_fn(rand_ctx) < SLICE_SELECT_ESCAPE_P) {
        /* Escape: jump 2..4 forward */
        int j = 2 + (int)(rand_fn(rand_ctx) * 3.0f);
        if (j < 2) j = 2;
        if (j > 4) j = 4;
        return (in->current_slice + j) & 7;
    }
    {
        float w_here = slice_select_weight_at(in->current_slice, eff_anchor);
        float p_repeat = 1.0f - w_here;
        if (rand_fn(rand_ctx) < p_repeat) {
            return in->current_slice;
        }
        int dir = (rand_fn(rand_ctx) < 0.5f) ? 1 : 7;
        return (in->current_slice + dir) & 7;
    }
}
