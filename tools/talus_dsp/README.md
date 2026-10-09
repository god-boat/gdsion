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
- `tests.py` runs T0 (the C++ matches the model to one pipe unit), T1-T14
  from the design, and IRON (Iron is bit-identical to its baseline). It prints
  one PASS/FAIL line per case and engine. The model runs a coarser note grid;
  T0, T14 and IRON are C++ only. `--engine cpp` skips the slow model runs.
- `calibrate.py` derives the level constants (`NOISE_FORCE`, `DIRECT_GAIN`,
  `OUTPUT_TRIM` and the rest) from renders. `--write` patches them into the
  channel source. Rebuild and repeat until the values settle (two or three
  passes).
- `render_presets.py` renders every shipped Talus preset, kit pads included,
  to `out/renders/*.wav` for the by-ear A/B, and prints each one's peak and
  idle time.
- `gen_membrane_modes.py` writes `src/dsp/membrane_modes.h`: the Bessel ratios
  and strike-position weights, computed with numpy alone.

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
- T14 timing on a laptop is noisy: the three voices run interleaved, best of
  five. Read it as a ratio to Iron, not as absolute numbers.
