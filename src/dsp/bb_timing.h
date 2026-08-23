#ifndef BB_TIMING_H
#define BB_TIMING_H

#include <stdint.h>

#define BB_TIMING_TRIGGER 0x01
#define BB_TIMING_BAR     0x02
#define BB_TIMING_START   0x04
#define BB_TIMING_STOP    0x08

typedef struct {
    int running;
    int tick_in_bar;
    int trigger_count;
} bb_timing_t;

void bb_timing_init(bb_timing_t *timing);

/* Process one MIDI realtime byte. Returns a BB_TIMING_* bitmask. Clock ticks
 * received while stopped advance no state and produce no events. */
int bb_timing_on_realtime(bb_timing_t *timing,
                          uint8_t status,
                          int ticks_per_trigger,
                          int *beat_position);

/* Exact playback duration for one of the eight slices. This is available from
 * the first render block and deliberately does not depend on observed ticks. */
float bb_timing_samples_per_trigger(float bpm,
                                    float loop_length_bars,
                                    int sample_rate);

#endif
