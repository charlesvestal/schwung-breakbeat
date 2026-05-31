# Breakbeat Phase 2 — Dual A/B Buffers

Date: 2026-05-31
Status: planned (execute when output tooling is stable)

## Goal

Make **A/B swap (note 60)** and the **B-slice row (notes 52–59)** actually
work by keeping both the A and B samples resident in memory simultaneously,
instead of mmap'ing one WAV at a time. This is the one structural change in the
original design doc.

Decisions locked in:
- **Each bank keeps its own length** (B plays at B Length, A at A Length). So
  per-bank `length`/`ticks_per_trigger` scaling, not one global value.
- Build/install/verify only when the relay is stable; verify the installed
  `.so` md5 matches the local dist build (this session has had silent
  edit-failures — always diff before committing).

## Current architecture (what we're replacing)

One sample slot lives directly on `breakbeat_t`:
`fd, map, map_size, data, total_frames, num_channels, audio_format,
bits_per_sample, slice_starts[8], slice_lengths[8]`, plus global timing
`active_length, ticks_per_trigger, samples_per_trigger`.

- `current_loop` ('A'/'B') is just a *label* of what's resident.
- `main_sample_path/main_length` = A; `alt_sample_path/alt_length` = B.
- Phrase swap (only when `phrase_bars > 0`) schedules `pending_sample_path +
  pending_loop + pending_main_length`, then at a bar boundary calls
  `apply_sample_path()` → `open_wav()` which **re-mmaps the single slot**,
  destroying the previous sample. `open_wav` also rescales
  `samples_per_trigger` for the new `expected_length`.
- Render reads `bb->data / bb->total_frames / bb->slice_* / bb->num_channels /
  bb->audio_format / bb->bits_per_sample` directly (~per-sample hot loop).

~93 references to these fields across the file.

## Target architecture

```c
typedef struct {
    int       fd;
    void     *map;
    size_t    map_size;
    void     *data;            /* points into map at the data chunk */
    uint32_t  total_frames;
    int       num_channels;
    int       audio_format;    /* WAV_FORMAT_PCM | WAV_FORMAT_FLOAT */
    int       bits_per_sample;
    uint32_t  slice_starts[8];
    uint32_t  slice_lengths[8];
    float     length;          /* this bank's active_length (1/4..8 bars) */
    char      path[BB_PATH_MAX];/* resolved path currently loaded, "" = empty */
} bb_sample_t;

/* on breakbeat_t: */
bb_sample_t samples[2];   /* [0] = A, [1] = B */
int engine_bank;          /* 0/1 — bank the generator is currently playing */
```

`current_loop` char stays as a derived label for status strings
(`engine_bank ? 'B' : 'A'`), to avoid touching every status snprintf.

### Per-trigger bank choice

At each fired trigger, decide which bank the *sounding* slice comes from:

```
bank = engine_bank;                       /* default: generator's bank */
if (perf has a held B-slice pad)   bank = 1;   /* B-slice row forces B */
else if (perf has a held A-slice)  bank = 0;   /* A-slice row forces A */
if (perf.ab_swap)                  bank ^= 1;  /* macro flips it momentarily */
if (samples[bank].data == NULL)    bank ^= 1;  /* fall back if bank empty */
```

Store the chosen bank for the duration of that slice in a new field
`render_bank` so the per-sample loop and the rate calc read the right bank.
(A held slice spans multiple render blocks, so `render_bank` must persist like
`current_slice`/`play_pos`, set at trigger-fire time.)

NB: today perf decode already distinguishes `BB_PAD_A_SLICE` vs
`BB_PAD_B_SLICE`, but the held-slice stack stores only the slice index, not the
bank. **Phase 2 needs the stack to remember the bank per entry.** Extend the
perf layer (pure, TDD'd):
- `bb_perf_slice_push(p, slice, bank)` / store `slice_bank[8]` alongside
  `slice_stack[8]`.
- `bb_perf_top_bank(p)` → bank of the top held slice (or -1).
- Keep dedupe keyed on (slice,bank) pair, or on slice alone? A and B slice 3
  are different pads → dedupe on (slice,bank). Update release to match both.
- `bb_perf_status_str` already takes `bank`; caller passes the resolved bank.

### Rate / timing with per-bank length

`rate = slice_lengths[bank][current_slice] / spt`, and `spt` derives from
`samples_per_trigger` which is measured globally from the clock. The *trigger
interval* itself (`ticks_per_trigger = 12 * length`) is per-bank now:

- The **engine** advances on its own bank's length (unchanged feel when no pads
  held).
- A **held B-slice** should retrigger at B's length. Because triggers are
  clock-tick driven in `on_midi` (`ticks_per_trigger` gates which ticks fire),
  switching the effective length mid-hold means choosing `ticks_per_trigger`
  from the sounding bank. Simplest correct approach: keep the engine's tick
  cadence for *when* triggers fire, but compute `rate` from the sounding bank's
  own `slice_lengths` and `spt` so the slice plays at the right pitch/length.
  Per-bank *retrigger cadence* (B at a different bar division than A) is a
  refinement; first cut = engine cadence, bank-correct slice bounds + rate.
  Document this limitation in the commit.

## Implementation steps

1. **Perf layer (TDD first, pure, host-tested).**
   - Add `slice_bank[8]` to `bb_perf_t`; `push(slice,bank)`,
     dedupe/release on (slice,bank); `bb_perf_top_bank()`.
   - Update all existing `bb_perf_slice_push(&p, n)` test calls to the new
     signature (A bank = 0) and add B-bank cases.
   - Run `tests/run_tests.sh` red→green before touching breakbeat.c.

2. **`bb_sample_t` + helpers in breakbeat.c.**
   - Define the struct; replace the inline WAV fields with `samples[2]`,
     `engine_bank`, `render_bank`.
   - Refactor `open_wav(wp, path, len)` → `load_sample(bb_sample_t *s, path,
     len)` filling a `bb_sample_t` (no global timing side-effects except its own
     `length`). Keep the mmap/madvise/validation logic verbatim.
   - `close_file` → `close_sample(bb_sample_t *s)`.
   - `apply_sample_path(bb, bank, path, len)` loads into `samples[bank]`.

3. **Eager B load.** Wherever A is applied (preset load, set_param
   A_sample_path, state restore, create), also load B into `samples[1]` from
   `alt_sample_path/alt_length`. B load failure is non-fatal (bank stays NULL →
   render falls back to A).

4. **Phrase swap re-points the bank index** instead of re-mmapping:
   - The bar-boundary handler currently calls `apply_sample_path(pending...)`.
     Replace with `engine_bank = pending_loop=='B' ? 1 : 0;` (+ snap slice 0).
   - Only reload via mmap when the *path* for that bank actually changed
     (user picked a new B sample), not on every swap.

5. **Render reads the chosen bank.**
   - At trigger fire: compute `render_bank` (see "per-trigger bank choice"),
     set `current_slice`, `play_pos = samples[render_bank].slice_starts[...]`.
   - Rate calc: `samples[render_bank].slice_lengths[current_slice] / spt`.
   - Per-sample loop: read `samples[render_bank].{data,num_channels,
     audio_format,bits_per_sample,total_frames}`. Bind these to locals once per
     block *after* the trigger sets render_bank (it's stable within a block
     because triggers fire at block start).

6. **Status string** uses the resolved bank's letter; `current_loop` derived.

7. **get_param/set_param/save-preset** keep A=main / B=alt path+length mapping;
   just route loads through `apply_sample_path(bank, …)`.

8. **destroy_instance** closes both banks.

9. **README + changelog**: A/B swap now functional; B-slice row plays real B;
   note the per-bank-cadence limitation if it ships in the first cut.

## Risks / watch-items

- **Hot-loop perf**: binding `bb_sample_t *s = &bb->samples[render_bank]` once
  per block keeps the per-sample loop as tight as today. Don't index
  `bb->samples[...]` inside the sample loop.
- **mmap lifetime**: two resident mmaps double the page footprint; both get
  `madvise(WILLNEED)`. Fine for typical break lengths.
- **Phrase swap + held pad interaction**: held pad's bank overrides engine_bank
  for the sounding slice but must NOT mutate engine_bank (release returns to the
  engine's groove). render_bank is transient; engine_bank persists.
- **Empty B**: every bank read guards `data == NULL` → fall back to A so the
  module never goes silent.
- **Verify discipline**: after edits, `git diff` must be non-empty for each file
  before commit; local dist `dsp.so` md5 must equal the installed device md5.

## Test plan

- Perf unit tests: B-bank push/dedupe/release, top_bank, status with B.
- On device: hold a B-slice pad over an A groove → hear B's slice; release →
  A groove resumes. Hold A/B-swap → engine flips bank; release → returns.
  Pick a different B sample → both still resident, no silence.
