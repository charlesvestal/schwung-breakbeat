# Breakbeat Generator for Schwung

A musically-anchored breakbeat slicer for Ableton Move (via Schwung).

Loads a WAV breakbeat, slices it into 8 equal parts, and recombines them per
trigger using biased-random selection. Designed for jungle, DnB, and breakbeat-
driven dance music: classic breaks (Amen, Funky Drummer, Think, Apache, etc.)
keep their kick-snare backbone via the **Anchor** knob, while **Roll**, **Fill**,
and **Phrase** add motion, fills, and multi-bar phrasing.

Demo: https://www.youtube.com/watch?v=vYf1vPt3pMc


## Controls

### Performance knobs (turn while playing)

| Knob | Range | What it does |
|---|---|---|
| **Complexity** | 0–100 | Probability that any given trigger picks a *random* slice instead of advancing in order. At 0, slices follow beat position (or Anchor if engaged). At 100, every non-stay trigger rolls a fresh random slice. |
| **Anchor** | 0–100 | Locks slice index to *beat position* in the bar. At 0, behavior is sequential advance with Complexity-driven random swaps. At 100, beats 1 and 3 (kick/snare) are protected from swaps and the no-swap fallback snaps to `beat_position`. |
| **Roll** | 0–100 | Temporal stickiness. At 0, every trigger is independent. At 100, most triggers either repeat the current slice, walk to the ±1 neighbor, or take a 5% escape-hatch jump. Produces the rolling jungle "1 2 3 1 2 3 4 5" feel and held-slice stutters. |
| **Fill** | 0–100 | Intensity of the *fill bar* modulation. Only meaningful when **Phrase** is non-Off. Modulates Complexity ↑, Roll ↓, Anchor ↓ on the last bar of every phrase. At 100, the fill bar throws out the groove rules entirely. |
| **Retrig 2x** | 0–100 | Per-bar probability of a 2x (half-slice) stutter on any given beat. |
| **Retrig 3x** | 0–100 | Per-bar probability of a 3x stutter. |
| **Retrig 4x** | 0–100 | Per-bar probability of a 4x stutter. |
| **Retrig 8x** | 0–100 | Per-bar probability of an 8x (16th-note micro-stutter) on any given beat. |

All four retrigger knobs are independent — any combination can be active at once. If multiple rates roll true on the same beat, one is chosen at random. 100% on any knob guarantees that rate fires on every beat; values represent true per-bar odds regardless of loop length.

### Settings (configure once, leave alone)

| Setting | Values | What it does |
|---|---|---|
| **A Sample** | filepath | Selects which WAV to slice. Opens file browser rooted at `/data/UserData/breakbeat-samples`, which contains a `Built-in/` folder and a `User Library/` symlink to your device's sample library. |
| **A Length** | enum | Trigger interval for A loop (1/4 bar to 8 bars). |
| **B Sample** | filepath | Selects the loop used for phrase fills. |
| **B Length** | enum | Trigger interval for B loop (1/4 bar to 8 bars). |
| **B Chance** | 0–100 | Probability of swapping to B Loop on the last bar of a phrase. |
| **Phrase** | enum | Multi-bar phrase length (Off, 2, 4, 8, 16 bars). |

### Meta

| Knob | Values | What it does |
|---|---|---|
| **Preset** | enum | Selects a dynamic preset from `presets/` folder. |
| **Save Preset** | toggle | Saves current settings as a new JSON file in `presets/`. |
| **Status** | read-only | Displays current playing loop, slice, and retrig status (e.g., `A_3_1x`). |

## Live Performance (MIDI pads)

On top of the generative engine there's a **live performance layer**: momentary
MIDI notes that override the engine while held. Send the module MIDI notes (from
the Move pads, an external controller, or a sequencer) — note-on engages an
effect, note-off releases it. Nothing latches: release everything and the
generative engine seamlessly resumes on the next tick.

> The DSP exposes the live state via the read-only `perf_status` param (e.g.
> `A:3 .5x REV` while held, empty otherwise). An on-screen overlay that surfaces
> this in the Signal Chain view is deferred — the chain/shadow UI is drawn by the
> host, so it needs a host-side hook rather than the module's own `ui.js`.
>
> **Note range:** the map below is the module's *raw MIDI note* assignment (base
> note 36, +8 per row). Note names use the convention where **C3 = 60** (middle C
> = note 60). How the Move's physical pads map onto these notes is a host-side
> concern still being finalised — for now drive it with raw notes from a
> controller or sequencer.

### MIDI map

| Notes | Names | Row | Function |
|---|---|---|---|
| **36–43** | C1–G1 | 0 (base) | A-slice 0–7 — play a slice of sample A (momentary) |
| **44–51** | G#1–D#2 | 1 | A-slice 0–7 — same as row 0 (kept for muscle memory) |
| **52–59** | E2–B2 | 2 | B-slice 0–7 — play a slice of sample B |
| **60–68** | C3–G#3 | 3+ | Macros (see below) |

#### Slice pads

| Note | Name | Slice |
|---|---|---|
| 36 / 44 | C1 / G#1 | A slice 1 |
| 37 / 45 | C#1 / A1 | A slice 2 |
| 38 / 46 | D1 / A#1 | A slice 3 |
| 39 / 47 | D#1 / B1 | A slice 4 |
| 40 / 48 | E1 / C2 | A slice 5 |
| 41 / 49 | F1 / C#2 | A slice 6 |
| 42 / 50 | F#1 / D2 | A slice 7 |
| 43 / 51 | G1 / D#2 | A slice 8 |
| 52–59 | E2–B2 | B slice 1–8 (from sample B) |

Hold a slice pad to jump to and re-trigger that slice in time with the clock.
**Last-note priority:** pressing a new slice pad overrides the current one;
releasing it falls back to whatever slice is still held, then to the engine.
Every press is momentary — a slice can only be held once, so a single note-off
always fully releases it (no stuck notes).

#### Macro pads

| Note | Name | Macro | While held |
|---|---|---|---|
| **60** | C3 | A/B swap | Flips the engine to the other sample bank (A↔B) while held |
| **61** | C#3 | Reverse | Slice plays backward, looping within its bounds |
| **62** | D3 | Randomize | Every trigger picks a fresh random slice |
| **63** | D#3 | Freeze | Latch the current slice and keep re-triggering it |
| **64** | E3 | ½× (half speed) | Slice plays an octave down **and** half as fast (re-triggers every other beat) |
| **65** | F3 | 2× (double speed) | Slice plays an octave up and twice as fast (re-triggers within the beat) |
| **66** | F#3 | Stutter 4× | Forces a 4× sub-slice retrigger |
| **67** | G3 | Stutter 8× | Forces an 8× sub-slice retrigger |
| **68** | G#3 | Reseed | One-shot: re-rolls the RNG and forces an immediate new slice pick |

All macros are momentary and stack. ½× and 2× held together cancel to 1×.
Reverse combines with any speed. Stutter layers on top of everything; holding
both Stutter pads gives 8× (the faster wins). A held slice pad always wins over
Randomize and Freeze (explicit beats automatic).

## Dynamic Presets & Custom Samples

Presets are no longer hardcoded in C. They are stored as `.json` files in the `presets/` directory. The module scans this directory on startup and when saving a new preset.

To add custom presets, place a JSON file in the `presets/` folder on the device and restart or save a preset to rescan.

Custom samples can be loaded via the file browser for **A Sample** and **B Sample**. The browser opens in `/data/UserData/breakbeat-samples`, with symlinks to built-in samples and your User Library.

## How the algorithm picks slices

Each trigger tick:

1. **Phrase modulation** — if Phrase is set and we're on the fill bar, Complexity is pushed up, Roll and Anchor are pushed down (proportional to Fill).
2. **Roll the dice** — random number `r ∈ [0,1)`:
   - If `r < (1 - Roll)` → **Move** branch (independent decision)
   - Otherwise → **Stay** branch (correlated with previous slice)
3. **Move branch:**
   - Compute swap probability: `p_swap = Complexity * weight_at(beat_position, Anchor)`
   - If we swap → uniform random slice 0..7
   - Else → play `beat_position` (the natural slice for this beat)
4. **Stay branch:**
   - 5% escape hatch: jump 2..4 forward
   - Otherwise: with probability `(1 - weight_at(current_slice, Anchor))` repeat current; else walk ±1

The **anchor weight curve** at Anchor=100 is `[0.0, 0.5, 1.0, 0.7, 0.0, 0.5, 1.0, 1.2]`:
- Slices 0 and 4 (beats 1 and 3) → weight 0 → never swap (kick/snare locked)
- Slice 7 (last 16th) → weight 1.2 → *more* likely to swap than baseline (fill territory)

Reseed happens automatically on transport start, so each play produces a fresh stochastic realization.

## Knob interactions worth knowing

- **Anchor=0, Roll=0, Phrase=Off** → original module behavior: sequential advance with Complexity-driven random swaps.
- **Anchor=100, Roll=0, Complexity=0** → straight playback of the break in beat order. At Length=0.5, this means slices 0, 2, 4, 6 (the structural beats only); at Length=0.25, slices 0..7 in order.
- **Anchor=100, Roll=100, Complexity=50** → camped on slice 0 with occasional ±1 walks and rare 5% escape jumps. Heavy stutter feel.
- **Anchor=80, Roll=70, Phrase=4, Fill=70** → bars 1–3 groove, bar 4 audibly opens up into a fill, bar 1 of the next phrase resets.
- **Phrase=2, Fill=100** → every other bar feels wild.

## Built-in Presets

Loaded dynamically from `src/presets/`:
- **1_calm**
- **2_mid**
- **3_frantic**

## UI Architecture

This module returns `ui_hierarchy` dynamically from C code as a compact JSON
string to enable the stock parameter list view in the Synth view of the host.
Do not use `ui_hierarchy` in `module.json` for instruments if you want this
behavior.

## Building

```bash
./scripts/build.sh    # cross-compiles dsp.so for aarch64 via Docker
./scripts/install.sh  # scp's to ableton@move.local
```

After install, restart Schwung on the device to load the module.

## Testing

Pure slice-selection and performance-layer logic are host-testable:

```bash
./tests/run_tests.sh  # compiles + runs test_slice_select.c and test_perf.c
```

## SSH setup (Mac → Move)

The install script uses `scp`/`ssh` to push files to the device. If you get `Permission denied (publickey)`:

```bash
# Generate a key on your build machine
ssh-keygen -t ed25519 -f ~/.ssh/move -N ""

# Add it to the Move (run from a machine that already has access)
echo "$(cat ~/.ssh/move.pub)" | ssh ableton@move.local \
  "mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys"

# Add a host entry so the key is picked up automatically
cat >> ~/.ssh/config << 'EOF'
Host move.local
    User ableton
    IdentityFile ~/.ssh/move
    StrictHostKeyChecking no
EOF
```

If the Move's host key has changed (after a firmware update or reset), clear the stale entry first:
```bash
ssh-keygen -R move.local
```

## Changelog

### v0.4.x — Live performance layer
- **Dual A/B buffers (Phase 2).** A and B samples are now both resident, so the A/B-swap macro (note 60) flips the engine between banks while held, and the B-slice row (notes 52–59) plays real slices of sample B. Each bank keeps its own length. (Held B slices retrigger at the engine's current cadence; per-bank cadence is a later refinement.)
- **Stutter split into two pads.** Stutter is now two separate momentary notes — 4× (note 66) and 8× (note 67) — instead of one velocity-sensitive pad. Holding both gives 8×. Reseed moves to note 68.
- **Momentary MIDI-pad performance system.** Slice pads (notes 36–59) and macro
  pads (60–67) override the generative engine while held; release to resume. See
  the [MIDI map](#midi-map) above.
- **Live state readout.** The `perf_status` param reports the held slice and
  active macros (e.g. `A:3 .5x REV`). A Signal-Chain on-screen overlay for this
  is deferred (needs a host-side hook).
- **½×/2× are true half/double speed**, not just pitch: ½× re-triggers the slice
  every other beat (plays twice as long), 2× re-triggers within the beat.
- **No stuck notes.** Repeated note-ons for the same slice dedupe to one held
  entry, so a single note-off always releases it.

### v0.4.0
- **Multi-rate retrigger.** Replaced the single Retrigger + Retrig Rate pair with four independent per-bar probability knobs (Retrig 2x / 3x / 4x / 8x). Any combination can be active simultaneously; if multiple rates fire on the same beat one is chosen at random. Probabilities are normalised correctly using the inverse binomial formula so 100% guarantees the rate fires on every beat and 5% means roughly 5% of bars. Old presets migrate automatically.
- **Sample preview.** Changing A Sample or Preset while transport is stopped now plays one full loop of the selected break immediately, using the current tempo knob value for rate. Lets you audition samples from the file browser without starting the transport.
- **User sample library access.** The A/B Sample file browser now exposes a `User Library/` folder alongside `Built-in/`, linked to `/data/UserData/UserLibrary/Samples`.
- **Tick-based timing rewrite.** Slice triggers now fire directly from MIDI 0xF8 clock ticks rather than a BPM phase accumulator. Playback rate is measured from the actual sample-counter distance between ticks — no BPM math, no drift. Falls back to the Move tempo-knob BPM when MIDI clock is unavailable.
- **Phrase-2 pre-scheduling fix.** For 2-bar phrases, B was never scheduled because the `bar_in_phrase == 0` boundary is never reached during normal playback. Fixed by pre-scheduling at transport reset.
- **Slice drift fix.** The no-swap branch now returns `beat_position` rather than `(current_slice+1) & 7`, preventing a random swap from permanently drifting all subsequent beats off-grid.
- **Retrigger bleed fix.** A mid-flight retrigger on the last beat of a bar no longer carries into the first beat of the incoming sample when a phrase swap occurs.

### v0.3.3
- Added diagnostic logging; renamed module display name to Breakbeat.

### v0.3.2
- **Module now loads the `1_calm` preset immediately on startup** instead of a missing hardcoded path.
- **Preset list sorted alphabetically** so index 0 is always `1_calm`.
- **State restore applies sample immediately** when transport is stopped.
- **Added `madvise(MADV_WILLNEED)`** after every `mmap` to pre-fault WAV pages before the audio thread touches them, preventing render-watchdog kills on sample swap.
