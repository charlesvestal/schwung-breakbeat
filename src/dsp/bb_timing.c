#include "bb_timing.h"

void bb_timing_init(bb_timing_t *timing) {
    if (!timing) return;
    timing->running = 0;
    timing->tick_in_bar = 0;
    timing->trigger_count = 0;
}

int bb_timing_on_realtime(bb_timing_t *timing,
                          uint8_t status,
                          int ticks_per_trigger,
                          int *beat_position) {
    if (!timing) return 0;
    if (ticks_per_trigger < 1) ticks_per_trigger = 1;

    if (status == 0xFA || status == 0xFB) {
        timing->running = 1;
        timing->tick_in_bar = 0;
        /* Slice zero starts immediately on Start. The first scheduled trigger
         * therefore advances to slice/beat one. */
        timing->trigger_count = 1;
        if (beat_position) *beat_position = 0;
        return BB_TIMING_START;
    }
    if (status == 0xFC) {
        timing->running = 0;
        timing->tick_in_bar = 0;
        timing->trigger_count = 0;
        return BB_TIMING_STOP;
    }
    if (status != 0xF8 || !timing->running) return 0;

    timing->tick_in_bar++;
    int events = 0;
    if ((timing->tick_in_bar % ticks_per_trigger) == 0) {
        if (beat_position) *beat_position = timing->trigger_count & 7;
        timing->trigger_count++;
        events |= BB_TIMING_TRIGGER;
    }
    if (timing->tick_in_bar >= 96) {
        timing->tick_in_bar = 0;
        events |= BB_TIMING_BAR;
    }
    return events;
}

float bb_timing_samples_per_trigger(float bpm,
                                    float loop_length_bars,
                                    int sample_rate) {
    if (bpm < 20.0f || bpm > 400.0f) bpm = 120.0f;
    if (loop_length_bars <= 0.0f) loop_length_bars = 1.0f;
    if (sample_rate <= 0) sample_rate = 44100;
    return ((float)sample_rate * 60.0f / bpm) * 4.0f
         * loop_length_bars / 8.0f;
}
