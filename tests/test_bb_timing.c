#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/dsp/bb_timing.h"

static int pass_count;
static int fail_count;

#define CHECK(cond, msg) do { \
    if (cond) pass_count++; \
    else { fail_count++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

int main(void) {
    bb_timing_t t;
    int beat = -1;
    bb_timing_init(&t);

    /* Move can emit clock continuously while stopped. It must stay silent. */
    for (int i = 0; i < 192; i++)
        CHECK(bb_timing_on_realtime(&t, 0xF8, 12, &beat) == 0,
              "stopped clock produces no events");
    CHECK(t.running == 0 && t.tick_in_bar == 0,
          "stopped clock does not advance phase");

    CHECK(bb_timing_on_realtime(&t, 0xFA, 12, &beat) == BB_TIMING_START,
          "Start enters running state");
    CHECK(t.running == 1 && beat == 0, "Start resets to slice zero");

    for (int i = 0; i < 11; i++)
        CHECK(bb_timing_on_realtime(&t, 0xF8, 12, &beat) == 0,
              "no early trigger");
    CHECK(bb_timing_on_realtime(&t, 0xF8, 12, &beat) == BB_TIMING_TRIGGER,
          "twelfth tick triggers");
    CHECK(beat == 1, "first scheduled trigger advances after start slice");

    CHECK(bb_timing_on_realtime(&t, 0xFC, 12, &beat) == BB_TIMING_STOP,
          "Stop exits running state");
    CHECK(bb_timing_on_realtime(&t, 0xF8, 12, &beat) == 0,
          "clock after Stop remains silent");

    /* A four-bar loop used to need two 48-tick trigger intervals (one bar)
     * before its rate was known. The rate must now be exact immediately. */
    float spt = bb_timing_samples_per_trigger(120.0f, 4.0f, 44100);
    CHECK(fabsf(spt - 44100.0f) < 0.01f,
          "four-bar loop rate is available immediately");
    CHECK(fabsf(bb_timing_samples_per_trigger(0.0f, 1.0f, 44100) - 11025.0f) < 0.01f,
          "invalid BPM uses safe 120 BPM fallback");

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
