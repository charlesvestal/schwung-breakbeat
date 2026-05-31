#ifndef BB_PERF_H
#define BB_PERF_H

/* ---------------------------------------------------------------------------
 * Live performance layer (Phase 1 — A-only).
 *
 * Pure, RT-safe, host-testable decision logic for the momentary MIDI-pad
 * performance system described in docs/plans/2026-05-31-performance-effects-
 * design.md. All state here is plain scalars mutated from on_midi() and read in
 * render_block(); no allocation, no I/O.
 *
 * "Momentary" = every pad is active only while held. Held slice pads override
 * the generative engine via a last-note-priority stack; macro pads set flags.
 * ------------------------------------------------------------------------- */

/* Pad note layout. The Move/Push grid is 4 rows × 8 cols with a +8 stride.
 * Kept as #defines so the map can be retuned in one place once the real note
 * range the host forwards is confirmed on-device. */
#define BB_PAD_BASE        36   /* note of row 0, col 0 */
#define BB_PAD_ROW_STRIDE  8    /* notes per row */

/* Macro indices (row 3+). Stutter 4×/8× are separate pads (velocity-independent),
 * so the macro set is 9 wide and spills one note past the 8-wide row — fine,
 * these are just MIDI notes. */
#define BB_MACRO_AB_SWAP   0
#define BB_MACRO_REVERSE   1
#define BB_MACRO_RANDOMIZE 2
#define BB_MACRO_FREEZE    3
#define BB_MACRO_HALF      4
#define BB_MACRO_DOUBLE    5
#define BB_MACRO_STUTTER4  6
#define BB_MACRO_STUTTER8  7
#define BB_MACRO_RESEED    8
#define BB_MACRO_COUNT     9

typedef enum {
    BB_PAD_NONE = 0,
    BB_PAD_A_SLICE,   /* index = slice 0..7 (notes 36-43 and 44-51)         */
    BB_PAD_B_SLICE,   /* index = slice 0..7 (notes 52-59; Phase 1 = A bank) */
    BB_PAD_MACRO      /* index = macro 0..8 (notes 60-68)                   */
} bb_pad_kind_t;

typedef struct {
    bb_pad_kind_t kind;
    int           index;   /* slice 0..7 or macro 0..7; -1 if NONE */
} bb_pad_t;

/* How render_block should choose the slice for a trigger tick. */
typedef enum {
    BB_RESOLVE_ENGINE = 0, /* normal generative pick                 */
    BB_RESOLVE_HELD,       /* a held slice pad; out_slice is the one  */
    BB_RESOLVE_FREEZE,     /* hold the engine's current_slice         */
    BB_RESOLVE_RANDOM      /* uniform-random slice                    */
} bb_resolve_mode_t;

typedef struct {
    int   slice_stack[8];  /* held slice pads, most-recent last  */
    int   slice_count;     /* depth of the stack (0 = none held) */

    int   reverse;         /* macro REVERSE held   */
    int   randomize;       /* macro RANDOMIZE held  */
    int   freeze;          /* macro FREEZE held     */
    int   ab_swap;         /* macro AB_SWAP held (Phase 2)  */

    int   half_held;       /* macro HALF held (guards rate_mult) */
    int   double_held;     /* macro DOUBLE held                  */
    float rate_mult;       /* product of ½×/2× holds; 1.0 = none */

    int   stutter4;        /* macro STUTTER4 held */
    int   stutter8;        /* macro STUTTER8 held */
    int   stutter_div;     /* effective division: 0 = off, else 8 (8× wins) or 4 */

    int   reseed_request;  /* one-shot: set on RESEED press, cleared by host */
} bb_perf_t;

/* Reset all performance state to inert (engine in full control). */
void bb_perf_init(bb_perf_t *p);

/* Decode a raw MIDI note into a pad. Returns {BB_PAD_NONE,-1} if out of range. */
bb_pad_t bb_perf_decode(int note);

/* Push a held slice pad (last-note priority). Duplicate slices each get their
 * own stack entry so rapid re-presses behave naturally. No-op if full. */
void bb_perf_slice_push(bb_perf_t *p, int slice);

/* Release a held slice pad: removes the most-recent stack entry matching
 * `slice`, preserving order of the rest. No-op if not present. */
void bb_perf_slice_release(bb_perf_t *p, int slice);

/* Top of the held-slice stack, or -1 if none held. */
int bb_perf_top_slice(const bb_perf_t *p);

/* Engage a macro on note-on. velocity is unused (kept for call-site symmetry). */
void bb_perf_macro_on(bb_perf_t *p, int macro, int velocity);

/* Release a macro on note-off. */
void bb_perf_macro_off(bb_perf_t *p, int macro);

/* Decide how the next trigger tick should pick its slice. When the return value
 * is BB_RESOLVE_HELD, *out_slice receives the held slice; otherwise it is left
 * unchanged. Priority: held slice > freeze > randomize > engine. */
bb_resolve_mode_t bb_perf_resolve(const bb_perf_t *p, int *out_slice);

/* 1 if any performance state is active (a slice pad held or any macro engaged),
 * i.e. the user is manually triggering something. Reseed is a one-shot and does
 * not count. */
int bb_perf_active(const bb_perf_t *p);

/* Build a short human-facing status string for the live overlay, e.g.
 * "A:3 .5x REV". `bank` is the sounding sample ('A'/'B'); `engine_slice` is the
 * generator's current 0-based slice, used when no slice pad is held. Slice
 * numbers are rendered 1-based for display. Writes "" and returns 0 when nothing
 * is being manually triggered (so the UI can hide the overlay). Returns the
 * written length. */
int bb_perf_status_str(const bb_perf_t *p, int engine_slice, char bank,
                       char *out, int len);

/* Trigger gating for the ½× macro. Call once per incoming clock trigger;
 * returns 1 if a new slice trigger should fire now, 0 to skip it:
 *   - rate_mult >= 1.0 (1× / 2×): always 1 (fire every clock trigger)
 *   - ½×  (rate_mult 0.5): 0,1,0,1,… so the held/engine slice plays across two
 *     clock intervals — "twice as long", i.e. half-time.
 * `*acc` is persistent caller state (init 0). The pitch/length change comes from
 * rate *= rate_mult separately; for 2× the slice repeats within the interval via
 * an intra-slice loop in the render path (see breakbeat.c), giving the design's
 * "plays in half the time then re-triggers". */
int bb_perf_trigger_fires(const bb_perf_t *p, float *acc);

#endif /* BB_PERF_H */
