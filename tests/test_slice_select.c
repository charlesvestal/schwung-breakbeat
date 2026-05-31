#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "../src/dsp/slice_select.h"

/* Deterministic seeded PRNG for tests. xorshift32. */
typedef struct { uint32_t state; } test_rng_t;

static float test_rand(void *ctx) {
    test_rng_t *r = (test_rng_t *)ctx;
    uint32_t x = r->state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->state = x;
    return (float)(x & 0x00FFFFFF) / (float)0x01000000;
}

static int g_pass = 0, g_fail = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (cond) { g_pass++; } else { \
        g_fail++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    float _a = (a), _b = (b); \
    if (fabsf(_a - _b) <= (eps)) { g_pass++; } else { \
        g_fail++; printf("FAIL: %s — got %f, expected %f (%s:%d)\n", msg, _a, _b, __FILE__, __LINE__); \
    } \
} while (0)

int main(void) {
    /* === weight_at: Anchor=0 → uniform 1.0 === */
    for (int i = 0; i < 8; i++) {
        float w = slice_select_weight_at(i, 0.0f);
        ASSERT_NEAR(w, 1.0f, 1e-5f, "weight_at(i, 0) should be 1.0");
    }

    /* === weight_at: Anchor=1 → locked curve === */
    {
        static const float expected[8] = {0.0f, 0.5f, 1.0f, 0.7f, 0.0f, 0.5f, 1.0f, 1.2f};
        for (int i = 0; i < 8; i++) {
            float w = slice_select_weight_at(i, 1.0f);
            ASSERT_NEAR(w, expected[i], 1e-5f, "weight_at(i, 1) should match locked curve");
        }
    }

    /* === weight_at: Anchor=0.5 → halfway between 1.0 and locked === */
    {
        static const float locked[8] = {0.0f, 0.5f, 1.0f, 0.7f, 0.0f, 0.5f, 1.0f, 1.2f};
        for (int i = 0; i < 8; i++) {
            float w = slice_select_weight_at(i, 0.5f);
            float expected = 0.5f * 1.0f + 0.5f * locked[i];
            ASSERT_NEAR(w, expected, 1e-5f, "weight_at(i, 0.5) interpolation");
        }
    }

    /* === apply_phrase: phrase_bars=0 → no modulation === */
    {
        slice_inputs_t in = {0};
        in.complexity = 0.5f; in.anchor = 0.7f; in.roll = 0.3f;
        in.phrase_bars = 0; in.fill = 1.0f; in.bar_in_phrase = 0;

        float c, a, r;
        slice_select_apply_phrase(&in, &c, &a, &r);
        ASSERT_NEAR(c, 0.5f, 1e-5f, "phrase off: complexity unchanged");
        ASSERT_NEAR(a, 0.7f, 1e-5f, "phrase off: anchor unchanged");
        ASSERT_NEAR(r, 0.3f, 1e-5f, "phrase off: roll unchanged");
    }

    /* === apply_phrase: bar 0 of 4-bar phrase → no modulation === */
    {
        slice_inputs_t in = {0};
        in.complexity = 0.5f; in.anchor = 0.7f; in.roll = 0.3f;
        in.phrase_bars = 4; in.fill = 1.0f; in.bar_in_phrase = 0;

        float c, a, r;
        slice_select_apply_phrase(&in, &c, &a, &r);
        ASSERT_NEAR(c, 0.5f, 1e-5f, "non-fill bar: complexity unchanged");
        ASSERT_NEAR(a, 0.7f, 1e-5f, "non-fill bar: anchor unchanged");
        ASSERT_NEAR(r, 0.3f, 1e-5f, "non-fill bar: roll unchanged");
    }

    /* === apply_phrase: fill bar (bar 3 of 4) with Fill=1 === */
    {
        slice_inputs_t in = {0};
        in.complexity = 0.5f; in.anchor = 0.7f; in.roll = 0.3f;
        in.phrase_bars = 4; in.fill = 1.0f; in.bar_in_phrase = 3;

        float c, a, r;
        slice_select_apply_phrase(&in, &c, &a, &r);
        ASSERT_NEAR(c, 1.0f, 1e-5f, "fill=1: complexity → 1.0");
        ASSERT_NEAR(a, 0.0f, 1e-5f, "fill=1: anchor → 0");
        ASSERT_NEAR(r, 0.0f, 1e-5f, "fill=1: roll → 0");
    }

    /* === apply_phrase: fill bar with Fill=0.5 → halfway === */
    {
        slice_inputs_t in = {0};
        in.complexity = 0.5f; in.anchor = 0.8f; in.roll = 0.6f;
        in.phrase_bars = 4; in.fill = 0.5f; in.bar_in_phrase = 3;

        float c, a, r;
        slice_select_apply_phrase(&in, &c, &a, &r);
        ASSERT_NEAR(c, 0.75f, 1e-5f, "fill=0.5: complexity halfway to 1.0");
        ASSERT_NEAR(a, 0.4f,  1e-5f, "fill=0.5: anchor halved");
        ASSERT_NEAR(r, 0.3f,  1e-5f, "fill=0.5: roll halved");
    }

    /* === select_next: Roll=0, Anchor=0, Complexity=0 → sequential advance ===
     * With Complexity=0, p_swap is always 0, so move-branch always falls through
     * to (current+1)&7. Roll=0 means stay-branch never fires. */
    {
        /* Sequential advance now returns beat_position (not current_slice+1) so
         * a random swap only affects one beat and the sequence stays aligned.
         * The slice to play at beat N is always beat_position=N when no swap. */
        test_rng_t rng = { .state = 12345 };
        slice_inputs_t in = {0};
        in.current_slice = 3;
        in.beat_position = 3;
        in.complexity = 0.0f; in.anchor = 0.0f; in.roll = 0.0f;
        in.phrase_bars = 0;

        int next = slice_select_next(&in, test_rand, &rng);
        ASSERT_TRUE(next == 3, "sequential: returns beat_position=3");

        in.current_slice = 7;
        in.beat_position = 7;
        next = slice_select_next(&in, test_rand, &rng);
        ASSERT_TRUE(next == 7, "sequential: returns beat_position=7");

        /* After a random swap (say at beat 4 → slice 1), the next beat still
         * plays its natural slice (beat_position=5), not current_slice+1=2. */
        in.current_slice = 1;  /* a random swap landed here */
        in.beat_position = 5;  /* but we are at beat 5 */
        next = slice_select_next(&in, test_rand, &rng);
        ASSERT_TRUE(next == 5, "sequential after swap: beat 5 plays slice 5, not slice 2");
    }

    /* === select_next: Roll=1, Anchor=0, current=4 -> walks should occur ===
     * Anchor=0 -> weight=1 for all slices -> p_repeat = 1 - 1 = 0.
     * Escape hatch is 5%; the other 95% goes through walk-+/-1.
     * From slice 4, walk lands on 3 or 5; escape jumps land on 6, 7, or 0
     * (current+2, +3, +4). None of those are 4. So `next == 4` should be ZERO. */
    {
        test_rng_t rng = { .state = 0x1234abcd };
        slice_inputs_t in = {0};
        in.current_slice = 4;
        in.beat_position = 4;
        in.complexity = 0.5f; in.anchor = 0.0f; in.roll = 1.0f;
        in.phrase_bars = 0;

        int repeat_count = 0, walk_neighbor = 0;
        const int N = 2000;
        for (int i = 0; i < N; i++) {
            int next = slice_select_next(&in, test_rand, &rng);
            if (next == 4) repeat_count++;
            if (next == 3 || next == 5) walk_neighbor++;
        }
        ASSERT_TRUE(repeat_count == 0, "Roll=1 Anchor=0 from slice 4: should never land on 4");
        ASSERT_TRUE(walk_neighbor > (int)(N * 0.80), "Roll=1 Anchor=0: >=80% are +/-1 walks");
    }

    /* === select_next: Roll=1, Anchor=1, current=0 -> repeat dominates ===
     * Anchor=1 + slice 0 -> weight=0 -> p_repeat=1.
     * 5% escape hatch fires; the other 95% should repeat slice 0.
     * Allow >=80% repeat to give the PRNG some headroom. */
    {
        test_rng_t rng = { .state = 0xfacefeed };
        slice_inputs_t in = {0};
        in.current_slice = 0;
        in.beat_position = 0;
        in.complexity = 0.5f; in.anchor = 1.0f; in.roll = 1.0f;
        in.phrase_bars = 0;

        int repeat_count = 0;
        const int N = 2000;
        for (int i = 0; i < N; i++) {
            int next = slice_select_next(&in, test_rand, &rng);
            if (next == 0) repeat_count++;
        }
        ASSERT_TRUE(repeat_count >= (int)(N * 0.80), "Roll=1 Anchor=1 slice=0: repeats >=80%");
    }

    /* === select_next: Roll=0, Anchor=0, Complexity=1 -> uniform random ===
     * All slices reachable; bucket counts roughly N/8 each. Allow +/-25%. */
    {
        test_rng_t rng = { .state = 0x55aa55aa };
        slice_inputs_t in = {0};
        in.current_slice = 0;
        in.beat_position = 0;
        in.complexity = 1.0f; in.anchor = 0.0f; in.roll = 0.0f;
        in.phrase_bars = 0;

        int counts[8] = {0};
        const int N = 8000;
        for (int i = 0; i < N; i++) {
            int next = slice_select_next(&in, test_rand, &rng);
            ASSERT_TRUE(next >= 0 && next < 8, "slice in 0..7");
            counts[next]++;
        }
        for (int i = 0; i < 8; i++) {
            ASSERT_TRUE(counts[i] >= 750 && counts[i] <= 1250, "uniform distribution per slice");
        }
    }

    /* === select_next: Phrase=4, bar_in_phrase=3, Fill=1 -> ignores Anchor & Roll ===
     * Inputs say Anchor=1, Roll=1 (would lock slice 0). But fill bar zeros both.
     * With effective_complexity=1, anchor=0, roll=0, move-branch always rolls
     * uniform 0..7. Slice 0 lands ~12.5% of the time, not 95%. */
    {
        test_rng_t rng = { .state = 0xc0ffee };
        slice_inputs_t in = {0};
        in.current_slice = 0;
        in.beat_position = 0;
        in.complexity = 1.0f; in.anchor = 1.0f; in.roll = 1.0f;
        in.phrase_bars = 4; in.fill = 1.0f; in.bar_in_phrase = 3;

        int repeat_count = 0;
        const int N = 2000;
        for (int i = 0; i < N; i++) {
            int next = slice_select_next(&in, test_rand, &rng);
            if (next == 0) repeat_count++;
        }
        ASSERT_TRUE(repeat_count <= (int)(N * 0.20), "fill bar: anchor lock released");
    }

    /* === select_next: Roll=0, Anchor=1, Complexity=0 -> slice locks to beat_position ===
     * Move branch always taken (Roll=0). p_swap=0 (Complexity=0).
     * With Anchor=1, the no-swap path always returns beat_position. */
    {
        test_rng_t rng = { .state = 0xa11ce1 };
        slice_inputs_t in = {0};
        in.complexity = 0.0f; in.anchor = 1.0f; in.roll = 0.0f;
        in.phrase_bars = 0;

        for (int bp = 0; bp < 8; bp++) {
            in.beat_position = bp;
            in.current_slice = (bp + 3) & 7;  /* deliberately misaligned */
            int next = slice_select_next(&in, test_rand, &rng);
            ASSERT_TRUE(next == bp, "Anchor=1 locks slice to beat_position");
        }
    }

    /* === select_next: Roll=0, Anchor=1, Complexity=0, beat_position=0 over many iters ===
     * Slice 0 should land on beat 1 every single time. */
    {
        test_rng_t rng = { .state = 0xb0a710d };
        slice_inputs_t in = {0};
        in.current_slice = 5;  /* misaligned start */
        in.beat_position = 0;
        in.complexity = 0.0f; in.anchor = 1.0f; in.roll = 0.0f;
        in.phrase_bars = 0;

        int slice0_count = 0;
        const int N = 1000;
        for (int i = 0; i < N; i++) {
            int next = slice_select_next(&in, test_rand, &rng);
            if (next == 0) slice0_count++;
            in.current_slice = next;  /* feedback */
        }
        ASSERT_TRUE(slice0_count == N, "Anchor=1 beat_position=0: kick lands every time");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
