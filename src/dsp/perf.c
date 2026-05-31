#include <stdio.h>
#include "perf.h"

void bb_perf_init(bb_perf_t *p) {
    if (!p) return;
    for (int i = 0; i < 8; i++) p->slice_stack[i] = 0;
    p->slice_count    = 0;
    p->reverse        = 0;
    p->randomize      = 0;
    p->freeze         = 0;
    p->ab_swap        = 0;
    p->half_held      = 0;
    p->double_held    = 0;
    p->rate_mult      = 1.0f;
    p->stutter_div    = 0;
    p->reseed_request = 0;
}

bb_pad_t bb_perf_decode(int note) {
    bb_pad_t r = { BB_PAD_NONE, -1 };
    int rel = note - BB_PAD_BASE;
    if (rel < 0 || rel >= 4 * BB_PAD_ROW_STRIDE) return r;
    int row = rel / BB_PAD_ROW_STRIDE;
    int col = rel % BB_PAD_ROW_STRIDE;
    switch (row) {
        case 0: /* live play — A slices, kept for backward compat */
        case 1: r.kind = BB_PAD_A_SLICE; r.index = col; break;
        case 2: r.kind = BB_PAD_B_SLICE; r.index = col; break;
        case 3: r.kind = BB_PAD_MACRO;   r.index = col; break;
        default: break;
    }
    return r;
}

void bb_perf_slice_push(bb_perf_t *p, int slice) {
    if (!p || slice < 0 || slice > 7) return;
    /* A slice occupies at most one stack entry. If it's already held (e.g. a
     * controller resent note-on without a note-off), remove the old entry and
     * re-append so it becomes the top (preserves last-note priority) without
     * leaving a duplicate that a single note-off couldn't clear — which would
     * otherwise latch the slice as a stuck note. */
    bb_perf_slice_release(p, slice);
    if (p->slice_count >= 8) return;   /* full; ignore (8 distinct slices max) */
    p->slice_stack[p->slice_count++] = slice;
}

void bb_perf_slice_release(bb_perf_t *p, int slice) {
    if (!p || p->slice_count <= 0) return;
    /* Remove the most-recent entry matching `slice`, shifting the rest down. */
    int found = -1;
    for (int i = p->slice_count - 1; i >= 0; i--) {
        if (p->slice_stack[i] == slice) { found = i; break; }
    }
    if (found < 0) return;
    for (int i = found; i < p->slice_count - 1; i++) {
        p->slice_stack[i] = p->slice_stack[i + 1];
    }
    p->slice_count--;
}

int bb_perf_top_slice(const bb_perf_t *p) {
    if (!p || p->slice_count <= 0) return -1;
    return p->slice_stack[p->slice_count - 1];
}

void bb_perf_macro_on(bb_perf_t *p, int macro, int velocity) {
    if (!p) return;
    switch (macro) {
        case BB_MACRO_AB_SWAP:   p->ab_swap   = 1; break;
        case BB_MACRO_REVERSE:   p->reverse   = 1; break;
        case BB_MACRO_RANDOMIZE: p->randomize = 1; break;
        case BB_MACRO_FREEZE:    p->freeze    = 1; break;
        case BB_MACRO_HALF:
            if (!p->half_held) { p->half_held = 1; p->rate_mult *= 0.5f; }
            break;
        case BB_MACRO_DOUBLE:
            if (!p->double_held) { p->double_held = 1; p->rate_mult *= 2.0f; }
            break;
        case BB_MACRO_STUTTER:
            p->stutter_div = (velocity >= BB_STUTTER_VEL_HI) ? 8 : 4;
            break;
        case BB_MACRO_RESEED:
            p->reseed_request = 1;
            break;
        default: break;
    }
}

void bb_perf_macro_off(bb_perf_t *p, int macro) {
    if (!p) return;
    switch (macro) {
        case BB_MACRO_AB_SWAP:   p->ab_swap   = 0; break;
        case BB_MACRO_REVERSE:   p->reverse   = 0; break;
        case BB_MACRO_RANDOMIZE: p->randomize = 0; break;
        case BB_MACRO_FREEZE:    p->freeze    = 0; break;
        case BB_MACRO_HALF:
            if (p->half_held) { p->half_held = 0; p->rate_mult *= 2.0f; }
            break;
        case BB_MACRO_DOUBLE:
            if (p->double_held) { p->double_held = 0; p->rate_mult *= 0.5f; }
            break;
        case BB_MACRO_STUTTER:
            p->stutter_div = 0;
            break;
        case BB_MACRO_RESEED:    /* one-shot; nothing to release */ break;
        default: break;
    }
}

bb_resolve_mode_t bb_perf_resolve(const bb_perf_t *p, int *out_slice) {
    if (!p) return BB_RESOLVE_ENGINE;
    if (p->slice_count > 0) {
        if (out_slice) *out_slice = p->slice_stack[p->slice_count - 1];
        return BB_RESOLVE_HELD;
    }
    if (p->freeze)    return BB_RESOLVE_FREEZE;
    if (p->randomize) return BB_RESOLVE_RANDOM;
    return BB_RESOLVE_ENGINE;
}

int bb_perf_active(const bb_perf_t *p) {
    if (!p) return 0;
    return (p->slice_count > 0)
        || p->reverse || p->randomize || p->freeze || p->ab_swap
        || p->half_held || p->double_held || (p->stutter_div > 0);
}

int bb_perf_trigger_fires(const bb_perf_t *p, float *acc) {
    if (!acc) return 1;
    float m = p ? p->rate_mult : 1.0f;
    if (m <= 0.0f) m = 1.0f;
    /* Only slow rates gate triggers; fast rates (2×) fire every clock and repeat
     * the slice within the interval instead. Cap the cadence at 1.0/trigger. */
    float cadence = (m < 1.0f) ? m : 1.0f;
    *acc += cadence;
    if (*acc >= 0.999f) {   /* 0.5+0.5 lands cleanly */
        *acc -= 1.0f;
        return 1;
    }
    return 0;
}

int bb_perf_status_str(const bb_perf_t *p, int engine_slice, char bank,
                       char *out, int len) {
    if (!out || len <= 0) return 0;
    out[0] = '\0';
    if (!bb_perf_active(p)) return 0;

    int slice = (p->slice_count > 0) ? bb_perf_top_slice(p) : engine_slice;
    if (slice < 0) slice = 0;

    int n = snprintf(out, (size_t)len, "%c:%d", bank, slice + 1); /* 1-based */
    if (n < 0) { out[0] = '\0'; return 0; }
    if (n >= len) return len - 1;

    /* Fixed token order so the readout is stable while pads are held. */
    if (p->half_held)        n += snprintf(out + n, (size_t)(len - n), " .5x");
    if (p->double_held)      n += snprintf(out + n, (size_t)(len - n), " 2x");
    if (p->reverse)          n += snprintf(out + n, (size_t)(len - n), " REV");
    if (p->freeze)           n += snprintf(out + n, (size_t)(len - n), " FRZ");
    if (p->randomize)        n += snprintf(out + n, (size_t)(len - n), " RND");
    if (p->stutter_div > 0)  n += snprintf(out + n, (size_t)(len - n), " ST%d", p->stutter_div);
    if (p->ab_swap)          n += snprintf(out + n, (size_t)(len - n), " A/B");

    if (n >= len) n = len - 1;
    return n;
}
