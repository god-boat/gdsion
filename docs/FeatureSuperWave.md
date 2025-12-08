# Super Wave Feature Documentation

## Overview

The Super Wave feature adds the ability for any FM operator to generate multiple stacked, detuned oscillator voices (2-16) from a single operator. This creates thick, chorused sounds similar to the "supersaw" found in synthesizers like the Roland JP-8000.

**Key Design Decision**: Implemented at the **operator level** rather than as a separate voice type, making it available to ALL FM configurations (1-4 operators, any algorithm, any chip type).

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `super_count` | 1-16 | 1 | Number of stacked voices. 1 = normal single oscillator (unchanged behavior) |
| `super_spread` | 0-1000 | 0 | Detune spread amount. Higher values = wider detuning between voices |

## Usage from GDScript

```gdscript
# Method 1: Using convenience method on SiONVoice
var voice = SiONVoice.create()
voice.set_operator_super_wave(0, 7, 200)  # operator 0, 7 voices, spread 200

# Method 2: Direct access to operator params
var op_params = voice.channel_params.get_operator_params(0)
op_params.super_count = 7
op_params.super_spread = 200
```

## How It Works

### Phase Distribution

When `super_count > 1`, the operator tracks multiple phase accumulators instead of just one. Each voice has a slightly different phase step based on the spread parameter:

```cpp
for (int i = 0; i < _super_count; i++) {
    // spread_factor ranges from -1.0 to +1.0, centered around 0
    float spread_factor = (float)(i - (_super_count - 1) * 0.5f) / ((_super_count - 1) * 0.5f);
    
    // Apply spread as a percentage of the base phase step
    int detune = (int)(_phase_step * spread_factor * _super_spread / 1000.0f);
    _super_phase_steps[i] = _phase_step + detune;
}
```

**Example with 7 voices and spread=200:**
- Voice 0: base_step - 20% detune (lowest pitch)
- Voice 1: base_step - 13.3% detune
- Voice 2: base_step - 6.7% detune
- Voice 3: base_step (center pitch)
- Voice 4: base_step + 6.7% detune
- Voice 5: base_step + 13.3% detune
- Voice 6: base_step + 20% detune (highest pitch)

### Output Summing

The outputs are summed in **linear space** (after log table lookup), not log space:

```cpp
int SiOPMOperator::get_super_output(int p_fm_input, int p_input_level, int p_am_level) {
    if (_super_count <= 1) {
        // Normal path - unchanged behavior
        int t = ((_phase + (p_fm_input << p_input_level)) & PHASE_FILTER) >> _wave_fixed_bits;
        int log_idx = get_wave_value(t) + _eg_output + p_am_level;
        return _table->log_table[log_idx];
    }
    
    // Super wave - sum linear outputs
    int sum = 0;
    for (int i = 0; i < _super_count; i++) {
        int t = ((_super_phases[i] + (p_fm_input << p_input_level)) & PHASE_FILTER) >> _wave_fixed_bits;
        int log_idx = get_wave_value(t) + _eg_output + p_am_level;
        sum += _table->log_table[log_idx];
    }
    // Normalize by sqrt(n) to maintain consistent perceived loudness
    return (int)(sum / sqrt((double)_super_count));
}
```

**Why linear summing matters**: The log table converts amplitude values. Adding in log space would multiply amplitudes (wrong). We need to add the actual waveform values, so we must convert to linear first, sum, then the result goes through the rest of the signal chain.

**Why sqrt(n) normalization**: With random starting phases (see below), the RMS power of summed signals grows proportionally to sqrt(n), not n. Dividing by sqrt(n) maintains roughly consistent perceived loudness regardless of voice count, while preserving the characteristic thickness from additional voices. Simple averaging (dividing by n) would make the sound quieter with more voices, which is not the expected supersaw behavior.

### Phase Randomization on Note-On

When `super_count > 1`, each super voice is initialized with a **random starting phase** on note-on. This is essential for the characteristic supersaw sound:

- **Without randomization**: All voices start in phase → sounds like one voice at attack, only gradually thickens as phases drift
- **With randomization**: Instant beating/chorus effect from the very first sample → rich, thick attack

This mimics the behavior of classic supersaw synthesizers like the Roland JP-8000.

### Envelope Sharing

All super wave voices share the **same envelope** from their parent operator. This is intentional - you want one cohesive sound that swells and fades together, not 7 independent envelopes.

---

## Files Modified

### 1. `gdsion/src/chip/siopm_operator_params.h`

Added parameter storage for serialization:

```cpp
// Super wave parameters.
// Number of stacked voices [1-16]. 1 = normal single oscillator.
int super_count = 1;
// Detune spread amount [0-1000]. Higher values = wider detuning.
int super_spread = 0;
```

Added getters/setters:

```cpp
int get_super_count() const { return super_count; }
void set_super_count(int p_value) { super_count = (p_value < 1) ? 1 : ((p_value > 16) ? 16 : p_value); }
int get_super_spread() const { return super_spread; }
void set_super_spread(int p_value) { super_spread = (p_value < 0) ? 0 : ((p_value > 1000) ? 1000 : p_value); }
```

### 2. `gdsion/src/chip/siopm_operator_params.cpp`

- Updated `initialize()` to set defaults
- Updated `copy_from()` to copy super wave params
- Updated `_to_string()` for debugging
- Bound methods and properties for GDScript access

### 3. `gdsion/src/chip/channels/siopm_operator.h`

Added state variables:

```cpp
// Super wave parameters.
static const int MAX_SUPER_VOICES = 16;
int _super_phases[MAX_SUPER_VOICES] = {};
int _super_phase_steps[MAX_SUPER_VOICES] = {};
int _super_count = 1;
int _super_spread = 0;

void _update_super_phase_steps();
```

Added public interface:

```cpp
// Super wave.
int get_super_count() const { return _super_count; }
int get_super_spread() const { return _super_spread; }
void set_super_wave(int p_count, int p_spread);
int get_super_output(int p_fm_input, int p_input_level, int p_am_level);
```

### 4. `gdsion/src/chip/channels/siopm_operator.cpp`

**New methods added:**

- `set_super_wave(count, spread)` - Configure super wave parameters
- `_update_super_phase_steps()` - Calculate per-voice phase steps with spread distribution
- `get_super_output(fm_input, input_level, am_level)` - Generate summed output from all voices

**Modified methods:**

- `tick_pulse_generator()` - Now advances all super phases when super_count > 1
- `note_on()` - Resets all super phases to the initial phase
- `_update_pitch()` - Calls `_update_super_phase_steps()` when pitch changes
- `set_operator_params()` - Loads super wave settings from params
- `get_operator_params()` - Saves super wave settings to params
- `initialize()` - Resets super wave state

### 5. `gdsion/src/chip/channels/siopm_channel_fm.cpp`

Refactored all standard process functions to use `get_super_output()`:

| Function | Description |
|----------|-------------|
| `_process_operator1_lfo_off` | 1-operator, no LFO |
| `_process_operator1_lfo_on` | 1-operator, with LFO |
| `_process_operator2` | 2-operator FM |
| `_process_operator3` | 3-operator FM |
| `_process_operator4` | 4-operator FM |
| `_process_analog_like` | Analog-style dual oscillator |

**Before:**
```cpp
ope0->tick_pulse_generator();
int t = ((ope0->get_phase() + (in_pipe->value << _input_level)) & SiOPMRefTable::PHASE_FILTER) >> ope0->get_wave_fixed_bits();

int log_idx = ope0->get_wave_value(t);
log_idx += ope0->get_eg_output() + (_amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift());
output = _safe_log_lookup(_table, log_idx);
```

**After:**
```cpp
ope0->tick_pulse_generator();
int am_level = _amplitude_modulation_output_level >> ope0->get_amplitude_modulation_shift();
output = ope0->get_super_output(in_pipe->value, _input_level, am_level);
```

**Unchanged functions** (special behaviors that don't translate to super wave):
- `_process_pcm_lfo_off` / `_process_pcm_lfo_on` - PCM sample playback
- `_process_ring` - Ring modulation (multiplies two wave values)
- `_process_sync` - Hard sync (resets phase based on another oscillator)

### 6. `gdsion/src/sion_voice.h`

Added convenience method declaration:

```cpp
void set_operator_super_wave(int p_operator_index, int p_count, int p_spread);
```

### 7. `gdsion/src/sion_voice.cpp`

Added implementation:

```cpp
void SiONVoice::set_operator_super_wave(int p_operator_index, int p_count, int p_spread) {
    if (p_operator_index >= 0 && p_operator_index < channel_params->get_operator_count()) {
        Ref<SiOPMOperatorParams> op_params = channel_params->get_operator_params(p_operator_index);
        op_params->set_super_count(p_count);
        op_params->set_super_spread(p_spread);
    }
}
```

Bound for GDScript:

```cpp
ClassDB::bind_method(D_METHOD("set_operator_super_wave", "operator_index", "count", "spread"), &SiONVoice::set_operator_super_wave);
```

### 8. `gdsion/src/sion_driver.h` / `.cpp`

Added real-time mailbox methods for live parameter changes:

```cpp
void mailbox_set_fm_op_super_count(int p_track_id, int p_op_index, int p_value);
void mailbox_set_fm_op_super_spread(int p_track_id, int p_op_index, int p_value);
```

These enable thread-safe real-time updates from the main thread to the audio thread.

### 9. `gdsion/src/chip/channels/siopm_channel_fm.h` / `.cpp`

Added helper methods to set super wave params on the active operator:

```cpp
void set_operator_super_count(int p_value);
void set_operator_super_spread(int p_value);
```

---

## Backward Compatibility

When `super_count = 1` (the default), operators behave **exactly as before**. The `get_super_output()` method takes the fast path that performs the same calculation as the original inline code.

## Performance Considerations

- **super_count = 1**: No performance impact (same code path as before)
- **super_count > 1**: Roughly linear scaling with voice count (7 voices ≈ 7x the wave lookups per sample)

For typical usage (supersaw leads, pads), 5-9 voices is common. The performance cost is acceptable for the thick sound produced.

## UI Integration

The super wave parameters are exposed in `objects/voice_specs/base_specs.tres` as operator specs:

- **SWVC** (Super Voice Count): 1-16, default 1
- **SWSP** (Super Spread): 0-1000, default 0

Both parameters have `template_for = ["basic"]` so they appear in basic operator parameter views.

## Real-Time Control Example

```gdscript
# Modify super wave parameters in real-time during playback
driver.mailbox_set_fm_op_super_count(track_id, 0, 7)    # Operator 0: 7 voices
driver.mailbox_set_fm_op_super_spread(track_id, 0, 200) # Operator 0: spread 200
```

## Possible Future Enhancements

1. **Spread modulation** via LFO or envelope
2. **Mix curve** parameter (how voices are panned/mixed)
3. **Detune curve** parameter (linear vs exponential spread distribution)

