# Voice Stealing Click Removal Efforts

## The Problem

Persistent audible clicks during voice stealing (polyphony limit exceeded, reusing voices for new notes).

### Root Cause Diagnosis

**Symptom**: Sharp clicking sounds when playing rapid notes at maximum polyphony.

**Identified Cause**: "Envelope ATTACK is starting while RELEASE still has non-zero slope, causing second-derivative spikes at arbitrary intra-buffer sample positions."

Specifically:
- When a new note arrives while an operator is still releasing
- The envelope jumps from RELEASE (non-zero slope going UP to silence) to ATTACK (steep slope going DOWN to full volume)
- This instantaneous slope change creates a curvature discontinuity (second derivative spike)
- Additionally, phase resets introduce waveform discontinuities
- These discontinuities manifest as audible clicks regardless of absolute volume level

### Debug Evidence

Logs showed:
```
FM_CURVATURE_SPIKE idx=84 accel=219 delta=-47 eg_state=2 eg_out=1880
FM_EDGE_JUMP chan_ptr=... first=0 prev=-4323 delta=4323
FM_OP_NOTE_ON ... prev_state=3 prev_level=50 new_state=2 new_level=0
```

- Curvature spikes: Large second derivative changes
- Edge jumps: Hard discontinuities at buffer boundaries  
- State transitions: RELEASE → ATTACK with non-zero envelope levels

## What Worked: Operator-Level Envelope Deferral

### Solution: Deferred ATTACK + Phase Reset

**Core Idea**: Prevent instantaneous DSP state changes. Let the old sound finish its natural decay BEFORE starting the new sound.

**Implementation** (`SiOPMOperator::note_on()` and `tick_eg()`):

1. **Detect Voice Stealing**:
   ```cpp
   const bool envelope_audible = 
       (_eg_state != EG_OFF && _eg_level < SiOPMRefTable::ENV_BOTTOM);
   const bool treat_as_voice_steal = _force_voice_steal || envelope_audible;
   ```

2. **Defer ATTACK Transition**:
   ```cpp
   if (treat_as_voice_steal) {
       _eg_pending_state = EG_ATTACK;
       _eg_has_pending_state = true;
       _phase_pending_reset = true;
       _eg_fast_release = true;
       
       if (_eg_state != EG_RELEASE) {
           _shift_eg_state(EG_RELEASE);
       } else {
           _shift_eg_state(EG_RELEASE); // Re-apply for fast rate
       }
   }
   ```

3. **Fast Release** (~2-3ms instead of natural release tail):
   ```cpp
   if (_eg_fast_release) {
       // Table 16 = fastest (increment by 8 per cycle)
       _eg_increment_table = make_vector<int>(_table->eg_increment_tables[16]);
       _eg_timer_step = _table->eg_timer_steps[63];
   }
   ```

4. **Transition When Quiet** (~90% to silence):
   ```cpp
   constexpr int FAST_RELEASE_THRESHOLD = SiOPMRefTable::ENV_BOTTOM - 80; // ~750
   
   if (_eg_level >= threshold) {
       if (_phase_pending_reset) {
           _reset_note_phases();
       }
       _shift_eg_state(_eg_pending_state); // Now safe to ATTACK
   }
   ```

### Why This Works

**Sequencing vs Blending**: The fix uses **sequential** transition (old→quiet→new) rather than **crossfading** (old+new).

1. **Old note continues**: Envelope and phase keep their current values
2. **Fast decay**: ~2-3ms smooth decay to near-silence
3. **Transition point**: When quiet enough (~90% to silence), new note starts
4. **New note clean**: Phase reset and ATTACK happen at low volume

**Key Insight**: Both envelope slope AND waveform shape must be continuous. By deferring the phase reset until the envelope is quiet, we avoid clicks from both sources.

### Results

- ✅ Clicks eliminated for FM voices
- ✅ Works across all FM algorithms (1-4 operators)
- ✅ Fast response (~2-3ms latency, imperceptible)
- ✅ No "dropped note" feeling
- ✅ Works with supersaws and complex modulation

## What Did NOT Work

### 1. Buffer-Level Solutions

**Attempted**: Curvature clamping, sample gating, buffer smoothing

```cpp
// Example: Attempted to clamp acceleration at buffer output
if (accel > THRESHOLD) {
    accel = THRESHOLD; // Clamp the spike
}
```

**Why Failed**: 
- Clicks are caused by **generating** discontinuous waveforms
- Clamping the output is like putting a band-aid on a broken bone
- The discontinuity still exists in the waveform, just artificially limited
- User quote: "the clamp was active but the click was still audible"

### 2. Channel-Level Fade System

**Attempted**: `trigger_fade_out_fast()` and `trigger_fade_in()` at channel level

```cpp
// In SiOPMChannelBase
void trigger_fade_out_fast() {
    _fade_state = FADE_OUT_FAST;
    _fade_gain = 1.0;
}

// Then multiply output samples by _fade_gain
elem->value = std::lround(elem->value * _fade_gain);
_fade_gain *= FADE_OUT_FAST_COEFF;
```

**Why Failed**:
- Fade is **post-synthesis** - applied to the output buffer AFTER waveform generation
- DSP state changes (frequency, phase, envelope) happen at `note_on()` time
- The **new** waveform shape appears immediately in the synthesis calculation
- Even at low fade volume, the sudden frequency/phase change creates a discontinuity
- Analogy: Fading the volume doesn't prevent the radio from changing stations abruptly

### 3. Track-Level Deferred key_on

**Attempted**: Store pending note, fade channel, execute `note_on()` when fade completes

```cpp
// In SiMMLTrack::_key_on()
bool is_voice_stealing = _channel->is_note_on() && !_channel->is_idling();

if (is_voice_stealing && !_flag_no_key_on) {
    _deferred_key_on_pending = true;
    _deferred_key_on_note = _note;
    _channel->trigger_fade_out_fast();
    return; // Don't call note_on yet
}

// Later in _process_buffer()
if (_deferred_key_on_pending && _channel->is_ready_for_key_on()) {
    _execute_deferred_key_on(); // Now call note_on
}
```

**Why Failed for FM**:
- Same reason as #2: timing is wrong
- Even though we delay the `note_on()` call, when it DOES execute:
  - Phase gets reset immediately
  - Frequency changes immediately  
  - Envelope jumps to ATTACK immediately
- The fade only affects the OLD voice's output
- The NEW voice starts with full DSP state changes at once
- The operator-level fix is needed INSIDE `note_on()` itself

**Additional Bug**:
- Non-FM channels stopped playing after some notes
- Issue: `_key_on_counter` management was broken during deferred execution
- Counter reached 0 before actual key_on happened, causing `_toggle_key()` confusion

### 4. Fade-Aware Attack Start Level

**Attempted**: Start envelope attack from ~10% volume instead of silence

```cpp
if (_force_voice_steal) {
    constexpr int FADE_AWARE_START_LEVEL = 750; // ~10% volume
    _eg_level = FADE_AWARE_START_LEVEL;
}
_shift_eg_state(EG_ATTACK);
```

**Intended**: Make new waveform fade in from low volume to match track fade

**Status**: Implemented but not fully tested as standalone solution. May help reduce click magnitude but doesn't address root cause of phase/frequency discontinuities.

### 5. Channel Reset Prevention

**Attempted**: Avoid calling `channel->initialize()` when reusing tracks

```cpp
// In UserControlledPool._init_slot
if not needs_replacement:
    var can_reset_channel := slot.track.has_method("is_finished") and slot.track.is_finished()
    if can_reset_channel and slot.track.has_method("reset_channel"):
        slot.track.reset_channel()
```

**Attempted**: Fast-path in `SiMMLVoice::update_track_voice()` to skip `initialize_tone()`

```cpp
// Check if channel type already matches
if (existing_ch->get_channel_type() == settings->get_channel_type()) {
    // Update params in-place, skip initialize
    p_track->get_channel()->set_channel_params(channel_params, update_volumes);
}
```

**Why Helped**: Eliminated `FM_OP_INIT` spam in logs, reduced initialization-related clicks

**Why Insufficient**: Even without full reinit, `note_on()` still causes discontinuities

## Key Learnings

### 1. Post-Synthesis vs Pre-Synthesis Fixes

**Post-synthesis** (output manipulation):
- Fades, gains, filters applied to samples
- Cannot fix discontinuities in waveform generation
- Like adjusting TV volume to fix channel switching

**Pre-synthesis** (DSP state management):
- Controlling when/how frequency, phase, envelope change
- Can prevent discontinuities from being generated
- The **ONLY** effective approach for this problem

### 2. Timing of State Changes

The critical insight: **WHEN** does the DSP state change?

- ❌ Wrong: Change state at `note_on()`, fade output afterward
- ✅ Right: Fade output FIRST, change state when quiet

### 3. Envelope + Phase Both Matter

You cannot fix just one:
- Smooth envelope + phase discontinuity = click
- Smooth phase + envelope discontinuity = click  
- **Both must be continuous** (or change while inaudible)

### 4. "Quiet Enough" vs "Silent"

Waiting for full silence (ENV_BOTTOM = 832) caused ~10ms latency and "dropped notes" feeling.

Using 90% threshold (level 750) balances:
- Low enough to mask remaining discontinuity
- Fast enough for imperceptible latency (~2-3ms total)

### 5. Architecture Matters

Why operator-level works but channel/track-level doesn't:
- **Operators**: Control the actual waveform generation
- **Channels**: Aggregate operator outputs
- **Tracks**: Schedule and control channels

To prevent clicks in waveform generation, you must control the generators themselves.

## Implementation Simplification

### Original Complexity (5 State Variables)

The initial working implementation used 5 separate flags to track voice-stealing state:

```cpp
EGState _eg_pending_state = EG_OFF;     // What state to transition to
bool _eg_has_pending_state = false;     // Do we have pending work?
bool _phase_pending_reset = false;      // Should we reset phase?
bool _eg_fast_release = false;          // Use fast release rate?
bool _force_voice_steal = false;        // Channel-level hint
```

This worked but was fragile - removing any one flag caused issues like dropped notes or long fadeouts.

### Simplified Implementation (2 State Variables)

All 5 flags were really encoding the same thing: "We're in voice-steal mode". The simplification uses:

```cpp
EGState _deferred_attack_target = EG_OFF;  // EG_OFF = not deferring, EG_ATTACK = deferring
bool _is_voice_steal_hint = false;         // Channel-level hint (unchanged)
```

**Key Insights:**

1. **Single source of truth**: `_deferred_attack_target != EG_OFF` means "we're voice stealing"
2. **Implicit fast release**: If deferring, always use fast release (no separate flag needed)
3. **Implicit phase reset**: If deferring, always reset phase when transitioning (no separate flag needed)
4. **Tagged union pattern**: The enum value itself encodes both "are we deferring?" and "what to defer to?"

**Benefits:**
- Fewer state variables: 5 → 2
- Simpler logic: Single check instead of multiple boolean combinations
- Harder to break: Fewer opportunities for flag desynchronization
- Easier to understand: One question to ask ("Are we deferring?")

## Files Modified (Final Working Solution)

### Core FM Fix

1. **`gdsion/src/chip/channels/siopm_operator.h`**:
   - Added `_deferred_attack_target`: Single state variable encoding whether we're deferring (replaces 5 previous flags)
   - Added `_is_voice_steal_hint`: One-shot hint from channel level
   - Added `_reset_note_phases()`: Centralized phase reset logic

2. **`gdsion/src/chip/channels/siopm_operator.cpp`**:
   - Modified `note_on()`: Detect voice stealing, defer attack/phase reset by setting `_deferred_attack_target = EG_ATTACK`
   - Modified `tick_eg()`: Check for deferred state, transition when quiet (simplified to single check)
   - Modified `_shift_eg_state(EG_RELEASE)`: Use fast rate if `_deferred_attack_target != EG_OFF` (implicit fast release)
   - Added `_reset_note_phases()`: Centralized phase reset logic
   - Modified `reset()`: Clear voice-stealing state flags

3. **`gdsion/src/chip/channels/siopm_channel_fm.cpp`**:
   - Modified `note_on()`: Set `_is_voice_steal_hint` for operators

### Core Sampler Fix

7. **`gdsion/src/chip/channels/siopm_channel_sampler.h`**:
   - Added `_has_deferred_note_on`: Flag to track deferred note_on
   - Added `_deferred_wave_number`, `_deferred_sample_start_phase`, `_deferred_pitch_step`: Store deferred note parameters
   - Added `_execute_note_on_immediate()`: Extracted original note_on logic

8. **`gdsion/src/chip/channels/siopm_channel_sampler.cpp`**:
   - **CRITICAL BUG FIX**: Modified `set_wave_data()` to NOT set `_sample_data` (was incorrectly setting individual sample holder to table pointer)
   - **CRITICAL BUG FIX**: Modified `reset()` to NOT clear `_sample_data` (preserve across resets for voice stealing)
   - Modified `note_on()`: Detect voice stealing (envelope audible + valid sample data), defer sample change and envelope restart
   - Added `_execute_note_on_immediate()`: Contains original note_on implementation
   - Modified `_update_amp_envelope()`: Check for deferred note_on, execute when quiet enough
   - Modified `buffer_no_process()`: Continue updating amp envelope when deferred note_on is pending
   - Modified `reset()`: Clear deferred state variables

### Supporting Changes

4. **`gdsion/src/sequencer/simml_voice.cpp`**:
   - Fast-path for FM voices: Skip `initialize()` when channel type matches
   - Fast-path for other voices: Check channel type before reinitializing

5. **`objects/UserControlledPool.gd`**:
   - Only reset channel when track is truly finished, not during voice stealing

6. **`gdsion/src/chip/channels/siopm_channel_base.h`**:
   - Adjusted fade coefficients for smoother transitions

## Attempts That Were Reverted

The following were implemented but did NOT solve the problem and were reverted:

- Channel-level `FADE_OUT_FAST` system
- Track-level deferred `key_on()` mechanism  
- `is_ready_for_key_on()` threshold checks
- Fade-aware attack start level (may revisit)
- Buffer-level curvature clamping

## Sampler Channel Declick Implementation

### The Same Problem in Samplers

After successfully implementing operator-level declick for FM voices, sampler channels exhibited identical clicks during voice stealing. Investigation revealed:

**Why the existing click guard failed:**
- The sampler channel had a `_click_guard` system that applied post-synthesis fading
- When `note_on()` was called during voice stealing, it:
  1. Immediately stopped the click guard (`_stop_click_guard()`)
  2. Reset envelope to 0 (`_reset_amp_envelope()`)
  3. Changed sample data immediately
  4. Started new attack

This created the same instantaneous discontinuity as FM voices before the fix.

### Solution: Deferred Note-On for Samplers

Applied the same deferred execution pattern as operators:

**Implementation** (`siopm_channel_sampler.cpp`):

1. **Added Deferred State Variables** (in header):
```cpp
bool _has_deferred_note_on = false;
int _deferred_wave_number = -1;
int _deferred_sample_start_phase = 0;
double _deferred_pitch_step = 1.0;
```

2. **Modified `note_on()` to Detect Voice Stealing**:
```cpp
// Voice-stealing declick: defer note_on if we're currently playing audible audio.
// Check both envelope state and sample data validity to avoid clicks.
const bool envelope_audible = (_amp_stage != AMP_STAGE_IDLE && _amp_level > 0.1);
const bool treat_as_voice_steal = envelope_audible && _sample_data.is_valid();

if (treat_as_voice_steal) {
    // Store new note parameters
    _has_deferred_note_on = true;
    _deferred_wave_number = _wave_number;
    _deferred_sample_start_phase = _sample_start_phase;
    _deferred_pitch_step = _pitch_step;
    
    // Trigger fast release
    if (_amp_stage != AMP_STAGE_RELEASE) {
        _begin_amp_release();
    }
    _configure_amp_stage(0.0, 63); // Rate 63 = fastest
    return;
}

// Not voice stealing - execute immediately
_execute_note_on_immediate();
```

3. **Execute Deferred Note-On When Quiet** (in `_update_amp_envelope()`):
```cpp
if (_has_deferred_note_on && _amp_stage == AMP_STAGE_RELEASE && _amp_level < 0.1) {
    // Restore deferred parameters
    _wave_number = _deferred_wave_number;
    _sample_start_phase = _deferred_sample_start_phase;
    _pitch_step = _deferred_pitch_step;
    
    _has_deferred_note_on = false;
    _execute_note_on_immediate();
    return;
}
```

4. **Extracted Original Logic** to `_execute_note_on_immediate()`:
   - Contains the original `note_on()` implementation
   - Called either immediately (no voice steal) or deferred (voice steal)

### Key Differences from Operator Implementation

**Threshold**: 
- Operator: Uses `ENV_BOTTOM - 80` (~750/832 in log domain, ~90% to silence)
- Sampler: Uses `_amp_level < 0.1` (10% of full scale in linear domain)
- Both achieve the same goal: transition when quiet enough to mask discontinuities

**Fast Release**:
- Operator: Uses fastest increment table (table 16) with timer step 63
- Sampler: Uses rate 63 directly in `_configure_amp_stage(0.0, 63)`

**State Tracking**:
- Operator: Stores `_deferred_attack_target` (enum) + hint flag
- Sampler: Stores `_has_deferred_note_on` (bool) + all note parameters

### Critical Bug Fixes

#### Bug #1: Missing Envelope Updates in buffer_no_process()

**Symptom**: Channel goes completely silent and stays silent after first voice steal.

**Root Cause**: When voice stealing occurred but channel had no sample_data (old sample finished), `buffer()` would call `buffer_no_process()` which never updated the amp envelope. The amp level stayed at 1.0 forever, so the threshold check never triggered.

**Fix**: In `buffer_no_process()`, check if we have a deferred note_on. If so, explicitly update the amp envelope for each sample until the threshold is reached and the deferred note executes.

```cpp
void SiOPMChannelSampler::buffer_no_process(int p_length) {
    // If we have a deferred note_on waiting, continue updating envelope
    if (_has_deferred_note_on) {
        for (int i = 0; i < p_length; i++) {
            _update_amp_envelope();
            if (!_has_deferred_note_on) break; // Executed!
        }
        if (!_has_deferred_note_on) return;
    }
    // ... rest of no_process logic
}
```

#### Bug #2: set_wave_data() Incorrectly Clearing _sample_data

**Symptom**: Clicks still occur even after Bug #1 is fixed. Logs showed `_sample_data` was null during voice stealing even for long-playing samples.

**Root Cause**: The bug was in `set_wave_data()`:

```cpp
// BROKEN:
void SiOPMChannelSampler::set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) {
    _sampler_table = p_wave_data;
    _sample_data = p_wave_data;  // BUG! Sets individual sample holder to TABLE pointer
}
```

During voice stealing:
1. Old note playing with individual sample data (e.g., note 60's sample)
2. `update_track_voice()` called → `set_wave_data(sampler_table)`
3. **`_sample_data` overwritten with table pointer** (wrong type!)
4. `note_off()` called → `_sample_data` is invalid/null
5. `note_on()` called → sees no audio to fade → no defer → **click!**

The `_sample_data` member should ONLY hold individual sample references (from `_sampler_table->get_sample()`), never the table itself.

**Fix**: Don't set `_sample_data` in `set_wave_data()`:

```cpp
// FIXED:
void SiOPMChannelSampler::set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) {
    _sampler_table = p_wave_data;
    // NOTE: Don't set _sample_data here - it should only hold individual samples
}
```

**Secondary Fix**: Don't clear `_sample_data` in `reset()`:

```cpp
// In reset():
_sampler_table = _table->sampler_tables[0];
// NOTE: Don't clear _sample_data here - preserve it across resets for voice stealing declick.
// _sample_data = Ref<SiOPMWaveSamplerData>();  // REMOVED
```

This preserves the currently playing sample across track resets during voice stealing, allowing the deferred logic to correctly detect audio and perform the crossfade.

**Safety Analysis**:
- `_sample_data` is only legitimately set in `note_on()` via `_sampler_table->get_sample()`
- After `reset()`, `_is_idling = true` prevents stale `_sample_data` from being accessed
- The next `note_on()` will replace `_sample_data` with the correct sample
- No downstream code expects `set_wave_data()` to modify `_sample_data`

### Critical Bug Fix: Deferred Execution Timing (resolved by Bug #1)

**Initial implementation bug**: The deferred note_on check happened AFTER `_advance_amp_stage()`, which transitions RELEASE → IDLE. The check only looked for `_amp_stage == AMP_STAGE_RELEASE`, so it never executed:

```cpp
// BROKEN:
_advance_amp_stage(); // RELEASE becomes IDLE here
if (_has_deferred_note_on && _amp_stage == AMP_STAGE_RELEASE && ...) {
    // Never executes because stage is now IDLE!
}
```

**Symptom**: After filling the voice pool (10 notes), the 11th note (first voice steal) would trigger the bug. The sampler channel would go completely silent and stay silent until app restart.

**Fix**: Check for deferred note_on BEFORE stage advancement, while still in RELEASE:

```cpp
// FIXED:
if (_has_deferred_note_on && _amp_stage == AMP_STAGE_RELEASE && _amp_level < 0.1) {
    _execute_note_on_immediate();
    return;
}
// Now safe to advance stage
if (_amp_stage_samples_left <= 0) {
    _advance_amp_stage();
}
```

Also added safety net in IDLE case to catch any edge cases.

### Results

- ✅ Clicks eliminated for sampler voices
- ✅ Consistent with FM voice declick behavior
- ✅ ~2-3ms latency (imperceptible)
- ✅ Works across all sample types (loops, one-shots, slices)
- ✅ No permanent silence after voice stealing (bug fixed)

## Open Questions for Future Work

### For PCM Channels

PCM channels may need similar treatment if they exhibit voice-stealing clicks. The pattern is now established and can be applied to any channel type:

1. Detect voice stealing in `note_on()`
2. Defer sample/state changes
3. Fast release to quiet
4. Execute deferred changes when amplitude is low

### Performance Considerations

The fast release adds ~2-3ms latency. For most musical contexts this is imperceptible, but for ultra-low-latency scenarios:

- Could reduce threshold further (risk more clicks)
- Could make threshold user-configurable
- Could disable deferral for certain instruments

### Compatibility with Effects

How does deferred attack interact with:
- Pitch bends during voice stealing?
- LFO modulation state?
- Filter envelope state?

All appear to work correctly in testing but need comprehensive validation.

## Conclusion

**The working solution**: Operator-level deferred ATTACK + phase reset with fast release.

**Core principle**: Don't try to hide discontinuities - **prevent them from being generated** by deferring DSP state changes until the output is quiet enough to mask any remaining artifacts.

**Applicability**: This approach should generalize to any synthesis system with envelopes and oscillators. The key is identifying where the actual waveform generation happens and deferring state changes at that level.

