# Talus DSP harness

Verifies the Talus channel (`src/chip/channels/siopm_channel_talus.cpp`) without
a Godot build. The design and its test cases are in the app repo at
`docs/TalusPhysicalModelDesign.md`.

Needs MSVC (Visual Studio 2022) and Python with numpy. Run from the app repo
root with its venv:

```powershell
.venv\Scripts\python.exe gdsion\tools\talus_dsp\build.py
.venv\Scripts\python.exe gdsion\tools\talus_dsp\tests.py
```

## Files

- `build.py` compiles the real channel sources, Talus and Iron, against the
  headers in `stubs/` into `out/talus_render.exe`. It also builds
  `out/iron_baseline.exe` from Iron as committed at `--iron-rev` (default
  `HEAD`), so a change to a shared `dsp/` block shows up as an Iron diff.
- `render_main.cpp` drives a channel from a text script (commands listed at
  its top). The stub base class keeps the engine's idle gate, and the pipes
  carry the same integers the engine's do.
- `harness.py` builds those scripts (`Job`) and runs them. It reads the
  param slots and defaults from `siopm_talus_params.h`.
- `model.py` is a reference model in Python and numpy, written from the design.
  It runs the same scripts. It reads tuning constants from the channel source,
  so recalibrating never desyncs the two, but every formula is its own.
- `tests.py` runs T0 (the C++ matches the model to one pipe unit), T1-T17
  from the design, and IRON (Iron is bit-identical to its baseline). It prints
  one PASS/FAIL line per case and engine. The model runs a coarser note grid;
  T0, T14 and IRON are C++ only. `--engine cpp` skips the slow model runs
  (about a minute and a half, against eight with them).
- `calibrate.py` derives the level constants (`NOISE_FORCE`, `DIRECT_GAIN`,
  `BODY_GAIN`, each model's `OUTPUT_TRIM` and the rest) from renders. `--write`
  patches them into the channel source. Rebuild and repeat until the values
  settle (two or three passes). Run it again after any change to an exciter or
  a resonator: the trims follow the default voice's peak.
- `render_presets.py` renders every shipped Talus preset, kit pads and racks
  included, to `out/renders/*.wav` for the by-ear A/B, and prints each one's
  peak and idle time. A drum plays a velocity ladder and a roll on one channel.
  A pitched preset plays a phrase and a chord, one channel per note. The WAVs
  are dry: a rack's effects are not rendered.
- `iron_tests.py` runs I1-I13 on Iron's handling noise: Scrape, Pick Bend and
  Fret Noise, and the channel contract they must keep (idle, segments,
  determinism, level). C++ only: Iron has no reference model.
- `render_iron.py` plays one riff through every shipped Iron preset, on this
  build and on the baseline, to `out/renders/Iron - *.wav` for the by-ear A/B.
- `gen_membrane_modes.py` writes `src/dsp/membrane_modes.h`: the Bessel ratios
  and strike-position weights, computed with numpy alone.
- `gen_bar_modes.py` writes `src/dsp/bar_modes.h`: the free-free bar's ratios
  and mode shapes, and the tuned bar's ratios.
- `mode_tables.py` is what both generators write through and what `model.py`
  reads the tables back with. A table is an array of `sion::dsp::ModeShape`
  (`src/dsp/modal_bank.h`).

`out/` is ignored. The folder carries a `.gdignore`, so the app's Godot
project never imports the renders.

## Gotchas

- With Variation at 0 every random draw is multiplied away, and the noise is a
  hash both sides compute the same way, so the C++ and the model agree to the
  sample. T0 checks this; if it fails, fix the disagreement before trusting any
  other case.
- The C++ output is truncated to whole pipe units, so measurements on it ignore
  anything within a few units of zero (`frequency()` only fits where the partial
  stands 16 units clear). T9 is exact on the model (1e-15) and within the two
  units truncation allows on the C++.
- T14 timing on a laptop is noisy: the voices run interleaved, best of five.
  Read it as a ratio to Iron, not as absolute numbers.
- The default stick's contact pulse (Hardness 60) has its first spectral null
  near 2.3 kHz. A test that compares mode levels above that sets Hardness 100,
  or a mode that sits on the null reads 30 dB low (T15 did).
- Each model has its own output trim, so compare two models' levels in head
  units: divide by `model.OUTPUT_TRIM[index]` (T16 does).
- A quiet tail cannot be measured on the C++ output: truncation to whole units
  shortens a decay fitted below about 16 units. Drive the thing harder (T17
  mutes a held string so the body rings from a high level).
- IRON fails while Iron's own source differs from `--iron-rev`. That is the
  case measuring a change to Iron; it passes again once the change is committed.
- A channel only idles on a render call that was silent throughout, so a case
  that needs an idle channel renders in short blocks (I12).
- To see one Iron noise by subtracting a render without it, nothing else may be
  random: Humanize, Timing, Pick Attack and Scrape at 0, and Voicing Single (the
  strum between a chord's strings takes a draw that Humanize does not scale).
