#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../src/dsp/perf.h"

static int g_pass = 0, g_fail = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (cond) { g_pass++; } else { \
        g_fail++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

#define ASSERT_EQ(a, b, msg) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a == _b) { g_pass++; } else { \
        g_fail++; printf("FAIL: %s — got %ld, expected %ld (%s:%d)\n", msg, _a, _b, __FILE__, __LINE__); \
    } \
} while (0)

#define ASSERT_NEAR(a, b, msg) do { \
    float _a = (a), _b = (b); \
    if (fabsf(_a - _b) <= 1e-6f) { g_pass++; } else { \
        g_fail++; printf("FAIL: %s — got %f, expected %f (%s:%d)\n", msg, _a, _b, __FILE__, __LINE__); \
    } \
} while (0)

int main(void) {
    /* === decode: row 0 (36-43) → A slices 0-7 (live play / backward compat) === */
    for (int n = 36; n <= 43; n++) {
        bb_pad_t p = bb_perf_decode(n);
        ASSERT_EQ(p.kind, BB_PAD_A_SLICE, "row0 is A slice");
        ASSERT_EQ(p.index, n - 36, "row0 index = note-36");
    }

    /* === decode: row 1 (44-51) → A slices 0-7 === */
    for (int n = 44; n <= 51; n++) {
        bb_pad_t p = bb_perf_decode(n);
        ASSERT_EQ(p.kind, BB_PAD_A_SLICE, "row1 is A slice");
        ASSERT_EQ(p.index, n - 44, "row1 index");
    }

    /* === decode: row 2 (52-59) → B slices 0-7 === */
    for (int n = 52; n <= 59; n++) {
        bb_pad_t p = bb_perf_decode(n);
        ASSERT_EQ(p.kind, BB_PAD_B_SLICE, "row2 is B slice");
        ASSERT_EQ(p.index, n - 52, "row2 index");
    }

    /* === decode: macros (60-68) → macros 0-8 === */
    for (int n = 60; n <= 68; n++) {
        bb_pad_t p = bb_perf_decode(n);
        ASSERT_EQ(p.kind, BB_PAD_MACRO, "60-68 is macro");
        ASSERT_EQ(p.index, n - 60, "macro index");
    }

    /* === decode: out of range → NONE === */
    {
        bb_pad_t lo = bb_perf_decode(35);
        bb_pad_t hi = bb_perf_decode(69);
        ASSERT_EQ(lo.kind, BB_PAD_NONE, "note 35 is none");
        ASSERT_EQ(hi.kind, BB_PAD_NONE, "note 69 is none");
    }

    /* === init: everything inert === */
    {
        bb_perf_t p;
        bb_perf_init(&p);
        ASSERT_EQ(p.slice_count, 0, "init: no held slices");
        ASSERT_EQ(bb_perf_top_slice(&p), -1, "init: top is -1");
        ASSERT_EQ(p.reverse, 0, "init: reverse off");
        ASSERT_EQ(p.randomize, 0, "init: randomize off");
        ASSERT_EQ(p.freeze, 0, "init: freeze off");
        ASSERT_EQ(p.stutter_div, 0, "init: stutter off");
        ASSERT_NEAR(p.rate_mult, 1.0f, "init: rate_mult 1.0");
        ASSERT_EQ(p.reseed_request, 0, "init: no reseed");
    }

    /* === slice stack: push/pop, last-note priority === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_slice_push(&p, 1, 0);
        bb_perf_slice_push(&p, 2, 0);
        bb_perf_slice_push(&p, 3, 0);
        ASSERT_EQ(p.slice_count, 3, "stack depth 3");
        ASSERT_EQ(bb_perf_top_slice(&p), 3, "top is 3");

        /* release a middle entry → top falls back to 3 */
        bb_perf_slice_release(&p, 2, 0);
        ASSERT_EQ(p.slice_count, 2, "depth 2 after release");
        ASSERT_EQ(bb_perf_top_slice(&p), 3, "top still 3");

        /* release the top → falls back to 1 */
        bb_perf_slice_release(&p, 3, 0);
        ASSERT_EQ(bb_perf_top_slice(&p), 1, "top falls back to 1");

        bb_perf_slice_release(&p, 1, 0);
        ASSERT_EQ(p.slice_count, 0, "empty");
        ASSERT_EQ(bb_perf_top_slice(&p), -1, "top -1 when empty");

        /* releasing absent slice is a no-op */
        bb_perf_slice_release(&p, 5, 0);
        ASSERT_EQ(p.slice_count, 0, "release absent no-op");
    }

    /* === slice stack: duplicate presses dedupe (momentary, no latching) ===
     * Real MIDI may resend note-ons without a matching note-off. A slice must
     * occupy at most ONE stack entry, so a single note-off always fully
     * releases it. Otherwise repeated note-ons latch the slice (stuck note). */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_slice_push(&p, 4, 0);
        bb_perf_slice_push(&p, 4, 0);
        bb_perf_slice_push(&p, 4, 0);   /* repeated note-ons */
        ASSERT_EQ(p.slice_count, 1, "repeated note-ons: single entry");
        bb_perf_slice_release(&p, 4, 0);
        ASSERT_EQ(p.slice_count, 0, "one note-off fully releases");
        ASSERT_EQ(bb_perf_top_slice(&p), -1, "not stuck after release");
    }

    /* === slice stack: re-press moves to top (keeps last-note priority) === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_slice_push(&p, 1, 0);
        bb_perf_slice_push(&p, 2, 0);
        bb_perf_slice_push(&p, 1, 0);   /* re-press 1 while 2 still held */
        ASSERT_EQ(p.slice_count, 2, "re-press: still two distinct slices");
        ASSERT_EQ(bb_perf_top_slice(&p), 1, "re-press brings slice to top");
        bb_perf_slice_release(&p, 1, 0);
        ASSERT_EQ(bb_perf_top_slice(&p), 2, "after releasing 1, 2 sounds");
    }

    /* === slice stack: A and B slice with same index are distinct pads === */
    {
        bb_perf_t p; bb_perf_init(&p);
        ASSERT_EQ(bb_perf_top_bank(&p), -1, "empty: top bank -1");
        bb_perf_slice_push(&p, 3, 0);   /* A slice 3 */
        bb_perf_slice_push(&p, 3, 1);   /* B slice 3 — different pad */
        ASSERT_EQ(p.slice_count, 2, "A3 and B3 both held");
        ASSERT_EQ(bb_perf_top_slice(&p), 3, "top slice 3");
        ASSERT_EQ(bb_perf_top_bank(&p), 1, "top bank is B");
        /* releasing B3 falls back to A3 */
        bb_perf_slice_release(&p, 3, 1);
        ASSERT_EQ(p.slice_count, 1, "B3 released, A3 remains");
        ASSERT_EQ(bb_perf_top_bank(&p), 0, "now top bank is A");
        /* releasing the wrong bank is a no-op */
        bb_perf_slice_release(&p, 3, 1);
        ASSERT_EQ(p.slice_count, 1, "release absent (3,B) no-op");
        bb_perf_slice_release(&p, 3, 0);
        ASSERT_EQ(p.slice_count, 0, "A3 released → empty");
        ASSERT_EQ(bb_perf_top_bank(&p), -1, "empty again");
    }

    /* === slice stack: same pad re-press dedupes within its bank === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_slice_push(&p, 5, 1);
        bb_perf_slice_push(&p, 5, 1);   /* repeated B5 note-on */
        ASSERT_EQ(p.slice_count, 1, "repeated B5 → single entry");
        ASSERT_EQ(bb_perf_top_bank(&p), 1, "still B");
        bb_perf_slice_release(&p, 5, 1);
        ASSERT_EQ(p.slice_count, 0, "one note-off clears B5");
    }

    /* === resolve: priority held > freeze > randomize > engine === */
    {
        bb_perf_t p; bb_perf_init(&p);
        int s = -99;
        ASSERT_EQ(bb_perf_resolve(&p, &s), BB_RESOLVE_ENGINE, "empty → engine");

        p.randomize = 1;
        ASSERT_EQ(bb_perf_resolve(&p, &s), BB_RESOLVE_RANDOM, "randomize → random");

        p.freeze = 1;
        ASSERT_EQ(bb_perf_resolve(&p, &s), BB_RESOLVE_FREEZE, "freeze beats randomize");

        bb_perf_slice_push(&p, 6, 0);
        s = -99;
        ASSERT_EQ(bb_perf_resolve(&p, &s), BB_RESOLVE_HELD, "held beats all");
        ASSERT_EQ(s, 6, "held slice surfaced");
    }

    /* === macro: ½× / 2× rate multiply and restore === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_HALF, 64);
        ASSERT_NEAR(p.rate_mult, 0.5f, "half → 0.5");
        bb_perf_macro_on(&p, BB_MACRO_DOUBLE, 64);
        ASSERT_NEAR(p.rate_mult, 1.0f, "half*double → 1.0");
        bb_perf_macro_off(&p, BB_MACRO_HALF);
        ASSERT_NEAR(p.rate_mult, 2.0f, "release half → 2.0");
        bb_perf_macro_off(&p, BB_MACRO_DOUBLE);
        ASSERT_NEAR(p.rate_mult, 1.0f, "release double → 1.0");
    }

    /* === macro: duplicate ½× note-on is idempotent === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_HALF, 64);
        bb_perf_macro_on(&p, BB_MACRO_HALF, 64);   /* stuck/running-status repeat */
        ASSERT_NEAR(p.rate_mult, 0.5f, "duplicate half stays 0.5");
        bb_perf_macro_off(&p, BB_MACRO_HALF);
        ASSERT_NEAR(p.rate_mult, 1.0f, "release returns to 1.0");
    }

    /* === macro: stutter 4× and 8× are separate pads (no velocity) === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_STUTTER4, 1);
        ASSERT_EQ(p.stutter_div, 4, "stutter4 → 4×");
        bb_perf_macro_off(&p, BB_MACRO_STUTTER4);
        ASSERT_EQ(p.stutter_div, 0, "release stutter4 → off");
        bb_perf_macro_on(&p, BB_MACRO_STUTTER8, 1);
        ASSERT_EQ(p.stutter_div, 8, "stutter8 → 8×");
        bb_perf_macro_off(&p, BB_MACRO_STUTTER8);
        ASSERT_EQ(p.stutter_div, 0, "release stutter8 → off");
    }

    /* === macro: both stutters held → 8× wins; releasing 8× falls back to 4× === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_STUTTER4, 1);
        bb_perf_macro_on(&p, BB_MACRO_STUTTER8, 1);
        ASSERT_EQ(p.stutter_div, 8, "both held → 8× wins");
        bb_perf_macro_off(&p, BB_MACRO_STUTTER8);
        ASSERT_EQ(p.stutter_div, 4, "release 8× → back to 4×");
        bb_perf_macro_off(&p, BB_MACRO_STUTTER4);
        ASSERT_EQ(p.stutter_div, 0, "release 4× → off");
    }

    /* === macro: reverse/randomize/freeze flag set & clear === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_REVERSE, 1);
        bb_perf_macro_on(&p, BB_MACRO_RANDOMIZE, 1);
        bb_perf_macro_on(&p, BB_MACRO_FREEZE, 1);
        ASSERT_EQ(p.reverse, 1, "reverse on");
        ASSERT_EQ(p.randomize, 1, "randomize on");
        ASSERT_EQ(p.freeze, 1, "freeze on");
        bb_perf_macro_off(&p, BB_MACRO_REVERSE);
        bb_perf_macro_off(&p, BB_MACRO_RANDOMIZE);
        bb_perf_macro_off(&p, BB_MACRO_FREEZE);
        ASSERT_EQ(p.reverse, 0, "reverse off");
        ASSERT_EQ(p.randomize, 0, "randomize off");
        ASSERT_EQ(p.freeze, 0, "freeze off");
    }

    /* === macro: reseed is a one-shot request on press === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_RESEED, 64);
        ASSERT_EQ(p.reseed_request, 1, "reseed requested on press");
        p.reseed_request = 0;   /* host consumes it */
        bb_perf_macro_off(&p, BB_MACRO_RESEED);
        ASSERT_EQ(p.reseed_request, 0, "reseed release does not re-request");
    }

    /* === active: false when nothing held, true when a pad/macro engaged === */
    {
        bb_perf_t p; bb_perf_init(&p);
        ASSERT_EQ(bb_perf_active(&p), 0, "inert → not active");
        bb_perf_slice_push(&p, 2, 0);
        ASSERT_EQ(bb_perf_active(&p), 1, "held slice → active");
        bb_perf_slice_release(&p, 2, 0);
        ASSERT_EQ(bb_perf_active(&p), 0, "released → not active");
        bb_perf_macro_on(&p, BB_MACRO_REVERSE, 1);
        ASSERT_EQ(bb_perf_active(&p), 1, "macro → active");
        bb_perf_macro_off(&p, BB_MACRO_REVERSE);
        ASSERT_EQ(bb_perf_active(&p), 0, "macro off → not active");
        /* reseed is one-shot, not a held state → not active */
        bb_perf_macro_on(&p, BB_MACRO_RESEED, 64);
        p.reseed_request = 0;
        ASSERT_EQ(bb_perf_active(&p), 0, "reseed one-shot → not active");
    }

    /* === status_str: empty when nothing manual === */
    {
        bb_perf_t p; bb_perf_init(&p);
        char buf[32];
        int n = bb_perf_status_str(&p, 3, 'A', buf, sizeof(buf));
        ASSERT_EQ(n, 0, "inert → length 0");
        ASSERT_TRUE(buf[0] == '\0', "inert → empty string");
    }

    /* === status_str: held slice, 1-based, with bank === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_slice_push(&p, 2, 0);   /* 0-based 2 → "3" */
        char buf[32];
        bb_perf_status_str(&p, 7, 'A', buf, sizeof(buf));
        ASSERT_TRUE(strcmp(buf, "A:3") == 0, "held slice → 'A:3'");
    }

    /* === status_str: macro-only uses engine slice (1-based) === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_REVERSE, 1);
        char buf[32];
        bb_perf_status_str(&p, 4, 'B', buf, sizeof(buf));  /* engine slice 4 → "5" */
        ASSERT_TRUE(strcmp(buf, "B:5 REV") == 0, "macro-only → 'B:5 REV'");
    }

    /* === status_str: held slice beats engine slice, with speed macro === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_slice_push(&p, 0, 0);                 /* → "1" */
        bb_perf_macro_on(&p, BB_MACRO_HALF, 64);
        char buf[32];
        bb_perf_status_str(&p, 6, 'A', buf, sizeof(buf));
        ASSERT_TRUE(strcmp(buf, "A:1 .5x") == 0, "held + half → 'A:1 .5x'");
    }

    /* === status_str: stutter shows division === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_STUTTER8, 1);
        char buf[32];
        bb_perf_status_str(&p, 0, 'A', buf, sizeof(buf));
        ASSERT_TRUE(strcmp(buf, "A:1 ST8") == 0, "stutter8 → 'A:1 ST8'");
    }

    /* === status_str: token order is stable (.5x/2x, REV, FRZ, RND, ST, A/B) === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_slice_push(&p, 1, 0);                       /* "2" */
        bb_perf_macro_on(&p, BB_MACRO_DOUBLE, 64);
        bb_perf_macro_on(&p, BB_MACRO_REVERSE, 1);
        bb_perf_macro_on(&p, BB_MACRO_FREEZE, 1);
        char buf[32];
        bb_perf_status_str(&p, 0, 'A', buf, sizeof(buf));
        ASSERT_TRUE(strcmp(buf, "A:2 2x REV FRZ") == 0, "ordered tokens");
    }

    /* === trigger gating: 1× fires every clock trigger === */
    {
        bb_perf_t p; bb_perf_init(&p);
        float acc = 0.0f;
        for (int i = 0; i < 8; i++)
            ASSERT_EQ(bb_perf_trigger_fires(&p, &acc), 1, "1x: fire every trigger");
    }

    /* === trigger gating: ½× fires every other trigger (half-time) === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_HALF, 64);   /* rate_mult 0.5 */
        float acc = 0.0f;
        int fires[6];
        for (int i = 0; i < 6; i++) fires[i] = bb_perf_trigger_fires(&p, &acc);
        ASSERT_EQ(fires[0], 0, "half: skip 1st");
        ASSERT_EQ(fires[1], 1, "half: fire 2nd");
        ASSERT_EQ(fires[2], 0, "half: skip 3rd");
        ASSERT_EQ(fires[3], 1, "half: fire 4th");
        ASSERT_EQ(fires[4], 0, "half: skip 5th");
        ASSERT_EQ(fires[5], 1, "half: fire 6th");
    }

    /* === trigger gating: 2× fires every clock (doubling is intra-slice) === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_DOUBLE, 64);   /* rate_mult 2.0 */
        float acc = 0.0f;
        for (int i = 0; i < 4; i++)
            ASSERT_EQ(bb_perf_trigger_fires(&p, &acc), 1, "double: fire every trigger");
    }

    /* === trigger gating: ½×+2× cancel to 1× === */
    {
        bb_perf_t p; bb_perf_init(&p);
        bb_perf_macro_on(&p, BB_MACRO_HALF, 64);
        bb_perf_macro_on(&p, BB_MACRO_DOUBLE, 64);   /* rate_mult 1.0 */
        float acc = 0.0f;
        for (int i = 0; i < 4; i++)
            ASSERT_EQ(bb_perf_trigger_fires(&p, &acc), 1, "half+double: fire every trigger");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
