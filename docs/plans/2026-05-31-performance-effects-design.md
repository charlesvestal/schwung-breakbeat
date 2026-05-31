# Breakbeat: Live Performance Effects Design

Date: 2026-05-31

## Goal

Add a **live performance layer** on top of the existing generative engine, so
the break can be played and mangled by hand from MIDI pads — finger-drumming
slices, freezing/repeating, randomizing, speed tricks, and A/B sample swaps —
without disturbing the algorithmic sequencer when no pads are held.

Today `on_midi()` already maps notes `36–43` to slices `0–7`, but it only sets
`play_pos`/`current_slice` once and then the generator immediately overwrites
the choice on the next tick. This design turns those one-shot jumps into a
proper, momentary, override-the-engine performance system and adds a second row
of B-sample slices plus a bank of effect modifiers.

## Design principles (decided)

These three answers drive every detail below:

1. **All momentary.** Every performance pad is active *only while held*.
   Note-on engages the effect; note-off releases it and control returns to the
   generative engine. No latched state to track, no stuck effects. This maps
   cleanly to MIDI note-on/note-off and is the most physical to play.

2. **Live overrides the engine.** While a performance pad is held, the
   generative slice picker (`slice_select_next`) is *bypassed* — the engine
   keeps running its clock, bars, and phrase counters, but the slice/rate it
   would have chosen is replaced by what your hands are doing. Release
   everything and the generator seamlessly resumes on the next tick.

3. **Instant hit, synced rate.** A pad press fires *immediately* (we jump
   `play_pos` to the slice the moment the note arrives in `on_midi`, not at the
   next tick) so playing feels responsive. But everything *rate-related* —
   re-trigger looping, half/double speed, stutter subdivisions — stays locked
   to the measured clock (`spt`, `samples_per_trigger`) so held notes never
   drift off-grid. You get tight timing without quantization latency.

## The pad map — four "playable pieces"

The Move pad grid is 4 rows × 8 columns. We treat each row as one coherent
performance instrument ("piece"), bottom-to-top:

```
Row 3 (top)   notes 60–67   MACROS      A/B swap · Reverse · Randomize · Hold/Freeze · ½× · 2× · Stutter · Reseed
Row 2         notes 52–59   B SLICES    slice 0 · 1 · 2 · 3 · 4 · 5 · 6 · 7   (from B sample)
Row 1         notes 44–51   A SLICES    slice 0 · 1 · 2 · 3 · 4 · 5 · 6 · 7   (from A sample)
Row 0 (base)  notes 36–43   LIVE PLAY   = current default behavior, see note
```

> **Note / to verify on-device:** the module currently receives `36–43` for the
> bottom row. The +8-per-row stride (44–51, 52–59, 60–67) is the natural Move /
> Push grid layout but must be confirmed against what Schwung actually forwards
> to the module. The base note and row stride are therefore defined as
> `#define`s (`BB_PAD_BASE 36`, `BB_PAD_ROW_STRIDE 8`) so the map can be
> retuned in one place. If only one row of pads is available in the host's synth
> view, the rows collapse to **banks** selected by a Macro pad instead (see
> "Fallback: single-row layout" below).

### Why this grouping

- **A slices (row 1)** and **B slices (row 2)** sit adjacent so you can
  finger-drum a break across *two* samples at once — e.g. kick from the Amen on
  row 1, snare from the Funky Drummer on row 2 — which is the classic two-break
  jungle move. This is the main reason A and B must both be resident in memory
  (see "Dual sample buffers" below).
- **Macros (row 3)** are the modifiers you ride with your other hand while the
  first hand plays slices. They layer on top of whatever is sounding — a held
  slice *or* the running generator.
- **Row 0** stays as the existing slice-trigger behavior for backward
  compatibility, but is upgraded to the new momentary/override semantics so old
  muscle memory still works.

## Effect catalog

Each effect, its pad, and its precise momentary behavior. "Source" = whichever
slice is currently sounding (a held A/B slice pad if any, otherwise the
generator's current slice).

### Slice pads — rows 0/1 (A) and 2 (B)

| Pad | Effect | On note-on (instant) | While held (synced) | On note-off |
|---|---|---|---|---|
| A slice 0–7 | Play A slice | Jump `play_pos` to that slice of sample **A**, sound now | Re-trigger the same slice every clock tick, in time | Pop from held-slice stack; revert to previous held slice or engine |
| B slice 0–7 | Play B slice | Same, from sample **B** | Same | Same |

Polyphony: **last-note priority with a small held stack.** Pressing a new slice
pad overrides the current one; releasing it falls back to whatever is still
held, and finally to the generator. This makes rolls and grace notes natural.

### Macro pads — row 3

| Pad | Effect | Behavior while held | Notes |
|---|---|---|---|
| 60 | **A/B swap** | Force the *source sample* to the opposite of the current `current_loop` for the engine's own slices | Lets you flip the running generator to the other break momentarily. Slice pads already pick their own sample, so this targets engine output. |
| 61 | **Reverse** | Playback direction flips: `play_pos` decrements; slice loops backward within its bounds | Combine with a held slice for reverse stutters |
| 62 | **Randomize** | Every tick picks a fresh uniform-random slice (momentary `complexity = 1.0`, `roll = 0`) | Overridden by a held slice pad (explicit beats random) |
| 63 | **Hold / Freeze** | Latches the *current* slice and keeps re-triggering it; the generator stops advancing | The "grab what's playing and stutter it" pad. Equivalent to a held slice pad targeting `current_slice`. |
| 64 | **Half speed (½×)** | `rate *= 0.5` | Power-of-two so it stays grid-locked. Slice sounds an octave down, plays twice as long |
| 65 | **Double speed (2×)** | `rate *= 2.0` | Octave up; slice plays in half the time then re-triggers |
| 66 | **Stutter (buzz)** | Momentary retrigger: forces `sub_slice_active`, `retrigger_divisions = 4` (or 8 with velocity ≥ 100) | Reuses the existing sub-slice machinery; no new DSP |
| 67 | **Reseed / Jump** | Re-seeds the RNG and forces one immediate generator re-pick | A "throw the dice" pad for instant variation without holding anything |

½× and 2× hold simultaneously → cancel to 1× (they multiply). Reverse stacks
with any speed. Stutter stacks with everything.

## State added to `breakbeat_t`

```c
/* ---- Live performance layer ---- */
int   perf_slice_stack[8];   /* held slice pads, most-recent last */
char  perf_slice_loop[8];    /* 'A' or 'B' per stack entry        */
int   perf_slice_count;      /* depth of the stack (0 = none held) */

int   perf_reverse;          /* macro 61 held */
int   perf_randomize;        /* macro 62 held */
int   perf_freeze;           /* macro 63 held: latch current_slice */
int   perf_ab_swap;          /* macro 60 held */
float perf_rate_mult;        /* product of ½×/2× holds; 1.0 = none */
int   perf_stutter_div;      /* 0 = off, else 4 or 8 (macro 66)    */
```

All of this is plain scalar state mutated from `on_midi()` and read in
`render_block()` — no allocation, RT-safe.

## Dual sample buffers (the one structural change)

Today only one WAV is mmap'd at a time (`data`, `total_frames`, …); A↔B is a
deferred swap at the bar boundary. Finger-drumming **both** breaks at once
requires both resident simultaneously and switchable per-sample.

Proposal: promote the single sample slot into a 2-element array indexed by loop:

```c
typedef struct {
    int fd; void *map; size_t map_size; void *data;
    uint32_t total_frames; int num_channels, audio_format, bits_per_sample;
    uint32_t slice_starts[8], slice_lengths[8];
} bb_sample_t;

bb_sample_t samples[2];   /* [0] = A, [1] = B */
```

`render_block` reads from `samples[active]` where `active` is chosen per
trigger: a held slice pad selects its own bank; otherwise the engine uses its
`current_loop`. The existing phrase-based A/B *swap-at-bar* logic keeps working
— it just changes which bank the engine points at instead of re-mmapping. B is
loaded once up front (on preset/`B_sample_path` change) rather than lazily at
the phrase boundary. This is the bulk of the implementation work; everything
else is small.

> If we want to keep scope minimal for a first cut, **Phase 1 can ship A-only
> slice pads** (rows 0/1) and defer the B row + dual buffers to Phase 2. The
> macros all work on the single active buffer with no architecture change.

## Integration points

### `on_midi()` — note-on / note-off

Replace the current `36–43` block. Decode `note → (row, col)` via
`BB_PAD_BASE`/`BB_PAD_ROW_STRIDE`:

- **Slice row (A/B):** note-on → push `{col, loop}` onto `perf_slice_stack`,
  set `play_pos` to that slice start in the chosen bank **immediately**, mark
  `pending` consumed so the next tick doesn't fight it. Note-off → remove that
  entry from the stack.
- **Macro row:** note-on → set the corresponding `perf_*` flag (½×/2× multiply
  into `perf_rate_mult`, stutter set `perf_stutter_div` from velocity). Note-off
  → clear it (½×/2× divide back out).
- Handle note-off as `0x80` *and* `0x90` with velocity 0 (running status).

### `render_block()` — apply the overrides

Inside the trigger-fire path (`BB_FIRE_TRIGGER` / tick handler):

```text
1. Determine the slice for this tick:
     if perf_slice_count > 0   -> slice = top-of-stack slice, bank = its loop
     else if perf_freeze       -> slice = current_slice (held)
     else if perf_randomize    -> slice = uniform random 0..7
     else                      -> slice = slice_select_next(...)   (normal engine)
2. Determine the source bank: held-pad's bank, else engine current_loop,
   flipped if perf_ab_swap is held.
3. Stutter: if perf_stutter_div, force sub_slice_active + that division
   (OR-ed with the preset retrigger logic).
```

In the per-sample render loop:

```text
rate_eff = rate * perf_rate_mult;          /* ½×/2× */
play_pos += perf_reverse ? -rate_eff : +rate_eff;
/* reverse: when play_pos < slice_start, wrap to slice_start + slice_len */
```

All reads (`data`, `total_frames`, `slice_*`) come from `samples[bank]`.

The engine's clock, `bar_counter`, phrase scheduling, and `spt` measurement run
untouched the whole time — that is what makes "release and the groove is exactly
where it should be" work.

## Conflict resolution & edge cases

- **Held slice pad vs Randomize/Freeze:** explicit slice pad wins (it's the most
  direct intent).
- **½× and 2× together:** multiply → 1.0 (no-op), per principle of stacking.
- **Reverse at slice start:** wrap to `slice_start + slice_len - 1` so the slice
  loops backward cleanly instead of running into the previous slice.
- **Pad held across a bar/phrase boundary:** the generator still advances its
  counters silently; the held slice keeps sounding; on release the engine is
  already at the correct bar position. No special handling needed.
- **B pad pressed before B sample loaded:** if `samples[1].data == NULL`, fall
  back to the A bank for that pad (and log once) rather than going silent.
- **Transport stopped:** performance pads still audition instantly (reuse the
  existing `preview_frames` path) so you can play it like an instrument without
  the transport running.
- **Velocity:** slice pads ignore velocity for now (could later scale gain);
  the stutter macro uses velocity ≥ 100 to pick 8× vs 4×.

## Fallback: single-row layout

If Schwung only forwards one row (notes `36–43`) to the synth, collapse to
**bank mode**: hold a dedicated "shift" pad (or a `perf_bank` param) to remap the
8 pads between A-slices / B-slices / macros. Less ideal for two-handed play but
keeps the whole feature on 8 pads. The effect catalog is unchanged; only the
note-decode in `on_midi()` differs. Worth confirming the real pad range before
committing to either path.

## Implementation phases

1. **Phase 1 — A-only performance layer (no architecture change).**
   Momentary slice pads on rows 0/1 from the single existing buffer; macros
   Reverse, Randomize, Freeze, ½×, 2×, Stutter, Reseed. Ships the whole *feel*
   with minimal risk. New state + `on_midi`/`render_block` edits only.
2. **Phase 2 — Dual A/B buffers.** Refactor the sample slot into `samples[2]`,
   load B eagerly, add the B slice row and the A/B-swap macro. Re-point the
   existing phrase swap logic at the bank index.
3. **Phase 3 — Polish.** Velocity-sensitive stutter/gain, optional `perf_bank`
   param + UI, status string shows live-vs-engine, host-testable performance
   logic extracted alongside `slice_select.c`.

## Open questions

- Exact pad note range the host forwards (drives rows-vs-banks). **Needs a
  quick on-device check.**
- Should Freeze (63) and a held slice pad be distinct, or is Freeze just "hold
  the engine's current slice" sugar? (Design assumes the latter.)
- Do we want a global gain/volume pad, or keep this purely slice-mangling?
- Should ½×/2× affect pitch only (resample) — which it does today via `rate` —
  or also be available as a tempo-synced "play the slice longer" that re-reads
  more of the sample? Current design = the former (musical, simple).
```

