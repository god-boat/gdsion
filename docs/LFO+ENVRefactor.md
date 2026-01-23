# LFO + Envelope Refactor Plan

## Goals
- Replace hard-coded routing with a unified modulation router.
- Make LFO and envelope outputs generic control sources that can target any param.
- Keep audio-thread safety and deterministic timing.
- Keep the codebase clean, single-source-of-truth, and easy to extend.

## Non-Goals
- Preserve legacy routing paths or exact legacy behavior.
- Run DSP in GDScript.
- Add UI features beyond basic plumbing.

## Proposed Architecture (Class Names)

### New C++ Module (engine-side)
Create a small modulation module (new folder suggested: `gdsion/src/modulation/`).

Core types:
- `SiModRouter` (new) in `gdsion/src/modulation/si_mod_router.{h,cpp}`
  - Owns slots, evaluates sources at control rate, applies to destinations.
- `SiModSource` (new interface) in `gdsion/src/modulation/si_mod_source.h`
  - `reset()`, `tick() -> float`, `set_rate()`.
- `SiModLFO` (new) in `gdsion/src/modulation/si_mod_lfo.{h,cpp}`
  - Wraps existing LFO wave table logic, outputs normalized value only.
- `SiModEnvelope` (new) in `gdsion/src/modulation/si_mod_envelope.{h,cpp}`
  - Wraps table playback, exposes normalized output.
- `SiModSlot` (new POD) in `gdsion/src/modulation/si_mod_slot.h`
  - `{ source_id, dest_id, depth, bias, curve, smoothing, bipolar, enabled, voice_scope }`.
- `SiModDestination` (new enum) in `gdsion/src/modulation/si_mod_destination.h`
  - Enumerates all modulatable params.
- `SiModBinding` (new) in `gdsion/src/modulation/si_mod_binding.{h,cpp}`
  - Maps `SiModDestination` -> concrete setter on track/channel/voice.

### Control Rate
- Router ticks at a fixed control interval (reuse envelope interval or add a new one).
- No per-sample modulation unless required by specific destinations.

## Files to Edit (Existing)

### Sequencer / Track
- `gdsion/src/sequencer/simml_track.h`
  - Remove fixed envelope destination fields.
  - Add a pointer/reference to `SiModRouter` (per-track or per-voice).
- `gdsion/src/sequencer/simml_track.cpp`
  - Replace `_buffer_envelope` updates (pitch/filter/amp/tone) with router ticks.
  - Use envelope playback only as a ModSource.

### LFO / Channel
- `gdsion/src/chip/channels/siopm_channel_base.h`
  - Add a unified param setter interface for modulation binding (if missing).
- `gdsion/src/chip/channels/siopm_channel_base.cpp`
  - Remove direct LFO-to-AM/PM wiring entry points.
- `gdsion/src/chip/channels/siopm_channel_fm.{h,cpp}`
  - Replace internal LFO outputs with `SiModLFO` outputs via router.
- `gdsion/src/chip/channels/siopm_channel_pcm.{h,cpp}`
  - Same as FM.
- `gdsion/src/chip/channels/siopm_channel_sampler.{h,cpp}`
  - Same as FM.

### Driver / API
- `gdsion/src/sion_driver.h`
  - Add mailbox APIs for mod slot updates (set/clear/update).
- `gdsion/src/sion_driver.cpp`
  - Implement mailbox updates to `SiModRouter` (RT-safe).

### GDScript Param System
- `objects/voice_specs/ParamSpec.gd`
  - Extend or clarify modulation metadata (dest enums, slot configs).
  - Keep engine-bound values in C++; GDScript only configures routing.

## Refactor Steps (Phased)

### Phase 1: Plumbing and Param Map
- Add `SiModDestination` enum and `SiModBinding` map.
- Add a per-track or per-voice `SiModRouter` instance.
- Files: `gdsion/src/modulation/*`, `gdsion/src/sequencer/simml_track.{h,cpp}`.

### Phase 2: Mod Router Core
- Implement slot storage and routing evaluation.
- Add RT-safe mailbox update calls in `SiONDriver`.
- Files: `gdsion/src/modulation/si_mod_router.{h,cpp}`, `gdsion/src/sion_driver.{h,cpp}`.

### Phase 3: LFO as ModSource
- Refactor LFO into `SiModLFO` output only.
- Remove direct AM/PM wiring in channel classes.
- Route LFO output through slots.
- Files: `gdsion/src/modulation/si_mod_lfo.{h,cpp}`, `gdsion/src/chip/channels/*`.

### Phase 4: Envelope as ModSource
- Refactor envelope tables into `SiModEnvelope` sources.
- Remove fixed envelope destinations in `SiMMLTrack`.
- Create envelope slots in router (pitch/filter/amp/tone become default config, not hardcoded).
- Files: `gdsion/src/modulation/si_mod_envelope.{h,cpp}`, `gdsion/src/sequencer/simml_track.{h,cpp}`.

### Phase 5: ParamSpec Integration
- Expose mod slot APIs to GDScript and map ParamSpec to destinations.
- Files: `objects/voice_specs/ParamSpec.gd`, `gdsion/src/sion_driver.{h,cpp}`.

### Phase 6: Cleanup
- Delete legacy envelope/LFO code paths.
- Consolidate remaining parameter updates into `SiModBinding`.

## Threading and Safety
- All modulation evaluation occurs on the audio thread.
- All updates from GDScript use mailbox or lock-free queue.
- Avoid per-buffer allocations in audio thread.

## Performance Guardrails
- Router runs only when at least one slot is active.
- Pre-allocate tables and avoid virtual dispatch in hot loops.
- Keep control-rate evaluation simple and branch-light.

## Testing and Verification
- Add a small internal test harness for LFO/Env sequences.
- Validate deterministic output for fixed seeds.
- Create a few regression sounds and compare rendered output.

## Risks
- Behavior changes are expected since legacy routing is removed.
- Parameter scaling and smoothing may alter perceived dynamics.
- Threading issues if mailbox updates race with audio thread.

## Deliverables
- Modulation module (`SiModRouter`, `SiModSource`, `SiModLFO`, `SiModEnvelope`).
- Destination binding map (`SiModBinding`).
- Updated track/channel processing paths.
- GDScript API to configure slots.

