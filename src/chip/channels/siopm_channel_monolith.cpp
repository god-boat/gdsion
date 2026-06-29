#include "siopm_channel_monolith.h"

#include <cmath>
#include <cstring>
#include <godot_cpp/core/class_db.hpp>
#include "chip/siopm_channel_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "templates/singly_linked_list.h"

static constexpr double TWO_PI = 6.283185307179586;
static constexpr double PI = 3.141592653589793;
static constexpr double HALF_PI = 1.5707963267948966;
static constexpr double PHASE_TO_RAD = TWO_PI / 4294967296.0;
static constexpr double PHASE_TO_NORM = 1.0 / 4294967296.0;
static constexpr double PIPE_PEAK = 8192.0;

// Motion filter / resonance tuning (internal main-layer SVF).
static constexpr double MOTION_FILTER_BASE_HZ = 1200.0; // sweep center
static constexpr double MOTION_FILTER_OCT = 3.0;        // +/- octaves at full motion
static constexpr double MOTION_RES_BASE_HZ = 900.0;     // fixed peak for growl/vowel
static constexpr double MOTION_RES_BASE_Q = 1.5;
static constexpr double MOTION_RES_Q_DEPTH = 3.0;
static constexpr double MOTION_RES_Q_MAX = 4.5;         // capped below self-oscillation

// Pitch drop: max envelope depth in pitch units (768 = 1 octave).
static constexpr double PITCH_DROP_MAX_UNITS = 768.0;

// Fast tanh approximation (Pade 3/3).
static inline double fast_tanh(double x) {
	if (x < -3.0) return -1.0;
	if (x > 3.0) return 1.0;
	double x2 = x * x;
	return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

// PolyBLEP residual for anti-aliased discontinuities.
// p_t: normalized phase [0,1), p_dt: normalized frequency (phase_inc / 2^32).
static inline double poly_blep(double p_t, double p_dt) {
	if (p_dt <= 0.0) {
		return 0.0;
	}
	if (p_t < p_dt) {
		double n = p_t / p_dt;
		return n + n - n * n - 1.0;
	} else if (p_t > 1.0 - p_dt) {
		double n = (p_t - 1.0) / p_dt;
		return n * n + n + n + 1.0;
	}
	return 0.0;
}

// ---------------------------------------------------------------------------
// Phase / frequency helpers
// ---------------------------------------------------------------------------

uint32_t SiOPMChannelMonolith::_freq_to_phase_inc(double p_freq) {
	if (p_freq <= 0.0 || _sample_rate <= 0.0) {
		return 0;
	}
	return (uint32_t)(p_freq / _sample_rate * 4294967296.0);
}

uint32_t SiOPMChannelMonolith::_pitch_to_phase_inc(double p_pitch_units) {
	// SiON pitch: 64 units per semitone, note 69 = 4416 = 440 Hz.
	double freq = 440.0 * std::pow(2.0, (p_pitch_units - 4416.0) / 768.0);
	return _freq_to_phase_inc(freq);
}

void SiOPMChannelMonolith::_update_phase_increments(double p_motion_mod) {
	double pitch = (double)_current_pitch;

	// Glide: smoothly interpolate toward target pitch.
	if (_glide_time > 0 && _has_previous_note) {
		_glide_current_pitch += (_target_pitch_f - _glide_current_pitch) * _glide_coeff;
		pitch = _glide_current_pitch;
	} else {
		pitch = _target_pitch_f;
		_glide_current_pitch = _target_pitch_f;
	}

	// Sub pitch with octave offset (0=−2oct, 1=−1oct, 2=unison) and pitch drop.
	// The pitch-drop envelope bends the sub anchor only (any shape, up to ~1 octave);
	// the main oscillators stay fixed so the drop reads as a sub transient.
	double sub_pitch = pitch + (double)(_sub_octave - 2) * 768.0;
	if (_pitch_env_level > 0.001) {
		sub_pitch += _pitch_env_level * ((double)_pitch_drop / 127.0) * PITCH_DROP_MAX_UNITS;
	}
	_sub_phase_inc = _pitch_to_phase_inc(sub_pitch);

	// Main oscillators with mass-based detune. Mass drift adds a slow detune
	// wobble to osc2 only (never the sub) for analog-like movement.
	double osc2_detune = _mass_detune_pitch * 0.5 + _mass_drift_value * 3.84; // +/-6 cents at full drift
	_osc1_phase_inc = _pitch_to_phase_inc(pitch - _mass_detune_pitch * 0.5);
	_osc2_phase_inc = _pitch_to_phase_inc(pitch + osc2_detune);

	// Precompute normalized frequency for polyBLEP.
	_osc1_dt = (double)_osc1_phase_inc * PHASE_TO_NORM;
	_osc2_dt = (double)_osc2_phase_inc * PHASE_TO_NORM;

	// Mass drift: subtle phase smear on osc2 (main layer only, never the sub).
	if (_mass_drift_value != 0.0) {
		int32_t smear = (int32_t)(_mass_drift_value * 0.01 * 4294967296.0);
		_osc2_phase += (uint32_t)smear;
	}

	// MOTION_PHASE: modulate osc2 phase offset via motion LFO.
	if (_motion_target_param == MOTION_PHASE) {
		int32_t phase_offset = (int32_t)(p_motion_mod * 536870912.0);
		_osc2_phase += (uint32_t)phase_offset;
	}
}

// ---------------------------------------------------------------------------
// Noise generator (32-bit LFSR)
// ---------------------------------------------------------------------------

double SiOPMChannelMonolith::_generate_noise() {
	_noise_state ^= _noise_state << 13;
	_noise_state ^= _noise_state >> 17;
	_noise_state ^= _noise_state << 5;
	return (double)(int32_t)_noise_state / 2147483648.0;
}

// ---------------------------------------------------------------------------
// Sub oscillator
// ---------------------------------------------------------------------------

double SiOPMChannelMonolith::_generate_sub_sample() {
	double t = (double)_sub_phase * PHASE_TO_NORM;
	double sample = 0.0;

	switch (_sub_shape) {
		case SUB_SINE:
			sample = std::sin((double)_sub_phase * PHASE_TO_RAD);
			break;

		case SUB_TRIANGLE: {
			sample = (t < 0.5) ? (4.0 * t - 1.0) : (3.0 - 4.0 * t);
		} break;

		case SUB_ROUNDED_SQUARE: {
			// Sine driven through saturation for rounded square edges.
			double s = std::sin((double)_sub_phase * PHASE_TO_RAD);
			sample = fast_tanh(s * 3.0);
		} break;

		case SUB_SATURATED_SINE: {
			double s = std::sin((double)_sub_phase * PHASE_TO_RAD);
			sample = fast_tanh(s * 2.0);
		} break;

		case SUB_OCTAVE_STACK: {
			double fund = std::sin((double)_sub_phase * PHASE_TO_RAD);
			double oct_below = std::sin((double)_sub_oct_phase * PHASE_TO_RAD);
			sample = fund * 0.6 + oct_below * 0.4;
		} break;

		case SUB_DUAL_HARMONIC: {
			double harmonic_mix = 0.22 + ((double)_shape_warp / 127.0) * 0.23;
			double fund = std::sin((double)_sub_phase * PHASE_TO_RAD);
			double second = std::sin((double)_sub_phase * PHASE_TO_RAD * 2.0);
			sample = fund * (1.0 - harmonic_mix) + second * harmonic_mix;
		} break;

		case SUB_PHASE_WARP: {
			double warp = 0.75 + ((double)_shape_warp / 127.0) * 1.25;
			double phase = (double)_sub_phase * PHASE_TO_RAD;
			sample = std::sin(phase + std::sin(phase) * warp);
		} break;

		default:
			sample = std::sin((double)_sub_phase * PHASE_TO_RAD);
			break;
	}

	return sample;
}

// ---------------------------------------------------------------------------
// Main oscillator (per voice) with polyBLEP anti-aliasing
// ---------------------------------------------------------------------------

double SiOPMChannelMonolith::_generate_osc_sample(uint32_t p_phase, int p_shape, double p_warp, double p_dt) {
	double t = (double)p_phase * PHASE_TO_NORM;
	double sample = 0.0;

	switch (p_shape) {
		case OSC_SAW: {
			// Sawtooth with optional warp (bends the ramp).
			double warped_t = t;
			if (p_warp > 0.01) {
				warped_t = std::pow(t, 1.0 + p_warp * 2.0);
			}
			sample = 2.0 * warped_t - 1.0;
			// PolyBLEP correction at the wrap discontinuity.
			sample -= poly_blep(t, p_dt);
		} break;

		case OSC_PULSE: {
			// Pulse with warp controlling duty cycle (0.15 to 0.85).
			double duty = 0.5 + p_warp * 0.35;
			sample = (t < duty) ? 1.0 : -1.0;
			// PolyBLEP at cycle start and duty crossing.
			sample += poly_blep(t, p_dt);
			double t_duty = t - duty;
			if (t_duty < 0.0) {
				t_duty += 1.0;
			}
			sample -= poly_blep(t_duty, p_dt);
		} break;

		case OSC_TRIANGLE: {
			double warped_t = t;
			if (p_warp > 0.01) {
				// Warp skews the triangle peak position.
				double peak = 0.5 - p_warp * 0.35;
				if (warped_t < peak) {
					sample = 2.0 * warped_t / peak - 1.0;
				} else {
					sample = 1.0 - 2.0 * (warped_t - peak) / (1.0 - peak);
				}
			} else {
				sample = (t < 0.5) ? (4.0 * t - 1.0) : (3.0 - 4.0 * t);
			}
			// Triangle is continuous so no BLEP needed.
		} break;

		case OSC_SINE_FOLD: {
			// Sine through wavefolder. Warp controls fold intensity.
			double s = std::sin((double)p_phase * PHASE_TO_RAD);
			double fold = 1.0 + p_warp * 6.0;
			sample = std::sin(s * fold * HALF_PI);
		} break;

		case OSC_FORMANT: {
			// Harmonic coloration via pitch-tracking ratio (not
			// frequency-stable formant synthesis). Warp sweeps the
			// harmonic ratio for growl and tonal color.
			double carrier = std::sin((double)p_phase * PHASE_TO_RAD);
			double formant_ratio = 2.0 + p_warp * 6.0;
			double formant = std::sin((double)p_phase * PHASE_TO_RAD * formant_ratio);
			double window = 0.5 + 0.5 * std::cos((double)p_phase * PHASE_TO_RAD);
			sample = carrier * 0.5 + formant * window * 0.5;
		} break;

		case OSC_SYNC: {
			// Hard sync emulation. Warp controls sync ratio.
			double sync_ratio = 1.0 + p_warp * 4.0;
			double sync_phase = std::fmod(t * sync_ratio, 1.0);
			sample = 2.0 * sync_phase - 1.0;
			// PolyBLEP at sync reset points.
			double sync_dt = p_dt * sync_ratio;
			sample -= poly_blep(sync_phase, sync_dt);
		} break;

		case OSC_DIGITAL: {
			// Quantized waveform (8-bit style). Warp controls bit depth.
			double s = std::sin((double)p_phase * PHASE_TO_RAD);
			double levels = 4.0 + (1.0 - p_warp) * 252.0;
			sample = std::round(s * levels) / levels;
		} break;

		case OSC_NOISE: {
			sample = _generate_noise();
		} break;

		default:
			sample = std::sin((double)p_phase * PHASE_TO_RAD);
			break;
	}

	return sample;
}

// ---------------------------------------------------------------------------
// Wavefold helper
// ---------------------------------------------------------------------------

double SiOPMChannelMonolith::_wavefold(double p_sample, double p_amount) {
	double x = p_sample * (1.0 + p_amount * 4.0);
	return std::sin(x * HALF_PI);
}

// ---------------------------------------------------------------------------
// Distortion
// ---------------------------------------------------------------------------

double SiOPMChannelMonolith::_apply_drive(double p_sample, int p_mode, double p_amount) {
	if (p_amount < 0.001) {
		return p_sample;
	}

	double drive = 1.0 + p_amount * 20.0;
	double out = p_sample;

	switch (p_mode) {
		case DRIVE_WARM:
			out = fast_tanh(out * drive);
			break;

		case DRIVE_CLIP:
			out = CLAMP(out * drive, -1.0, 1.0);
			break;

		case DRIVE_FOLD:
			out = _wavefold(out, p_amount);
			break;

		case DRIVE_TEAR: {
			// Asymmetric soft clip: positive half clips harder.
			double x = out * drive;
			if (x > 0.0) {
				out = 1.0 - std::exp(-x);
			} else {
				out = fast_tanh(x);
			}
		} break;

		case DRIVE_TUBE: {
			// Tube-like with even harmonics (asymmetric transfer function).
			double x = out * drive;
			double x_abs = std::fabs(x);
			out = (x > 0.0)
					? (1.0 - std::exp(-x_abs)) * 1.05
					: -(1.0 - std::exp(-x_abs * 0.9));
		} break;

		case DRIVE_DIGITAL: {
			// Bit-crush / sample-rate reduction feel.
			double levels = 2.0 + (1.0 - p_amount) * 254.0;
			out = std::round(out * drive * levels) / levels;
			out = CLAMP(out, -1.0, 1.0);
		} break;

		default:
			out = fast_tanh(out * drive);
			break;
	}

	return out;
}

// ---------------------------------------------------------------------------
// Amplitude envelope
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::_reset_amp_envelope() {
	_amp_stage = AMP_STAGE_IDLE;
	_amp_level = 0.0;
	_amp_stage_target_level = 0.0;
	_amp_stage_increment = 0.0;
	_amp_stage_samples_left = 0;
	_envelope_level = 0.0;
}

void SiOPMChannelMonolith::_start_amp_envelope() {
	_is_idling = false;
	_set_amp_stage(AMP_STAGE_ATTACK);
}

void SiOPMChannelMonolith::_begin_amp_release() {
	if (_amp_stage == AMP_STAGE_IDLE || _amp_stage == AMP_STAGE_RELEASE) {
		return;
	}
	_set_amp_stage(AMP_STAGE_RELEASE);
}

void SiOPMChannelMonolith::_advance_amp_stage() {
	switch (_amp_stage) {
		case AMP_STAGE_ATTACK: {
			bool needs_decay = (_amp_sustain_level < 128) || (_amp_decay_rate > 0);
			if (needs_decay) {
				_set_amp_stage(AMP_STAGE_DECAY);
			} else {
				_set_amp_stage(AMP_STAGE_SUSTAIN);
			}
		} break;
		case AMP_STAGE_DECAY: {
			_set_amp_stage(AMP_STAGE_SUSTAIN);
		} break;
		case AMP_STAGE_RELEASE: {
			_set_amp_stage(AMP_STAGE_IDLE);
			_declick_target = 0.0;
			_is_idling = false;
		} break;
		default:
			break;
	}
}

void SiOPMChannelMonolith::_set_amp_stage(AmplitudeStage p_stage) {
	_amp_stage = p_stage;

	switch (p_stage) {
		case AMP_STAGE_ATTACK: {
			_is_idling = false;
			_amp_level = CLAMP(_amp_level, 0.0, 1.0);
			_configure_amp_stage(1.0, _amp_attack_rate);
		} break;
		case AMP_STAGE_DECAY: {
			_is_idling = false;
			double sustain = (double)_amp_sustain_level * 0.0078125;
			_configure_amp_stage(sustain, _amp_decay_rate);
		} break;
		case AMP_STAGE_SUSTAIN: {
			_is_idling = false;
			_amp_stage_samples_left = 0;
			_amp_stage_increment = 0.0;
			_amp_level = (double)_amp_sustain_level * 0.0078125;
			_envelope_level = _amp_level;
		} break;
		case AMP_STAGE_RELEASE: {
			_is_idling = false;
			_configure_amp_stage(0.0, _amp_release_rate);
		} break;
		case AMP_STAGE_IDLE:
		default: {
			_amp_stage_samples_left = 0;
			_amp_stage_increment = 0.0;
			_amp_level = 0.0;
			_envelope_level = 0.0;
		} break;
	}
}

void SiOPMChannelMonolith::_configure_amp_stage(double p_target_level, int p_rate) {
	_amp_stage_target_level = CLAMP(p_target_level, 0.0, 1.0);
	double delta = _amp_stage_target_level - _amp_level;
	double delta_abs = std::fabs(delta);
	bool immediate = (p_rate <= 0) || (delta_abs < 0.0001);
	if (immediate) {
		_amp_level = _amp_stage_target_level;
		_amp_stage_samples_left = 0;
		_amp_stage_increment = 0.0;
		if (_amp_stage == AMP_STAGE_ATTACK || _amp_stage == AMP_STAGE_DECAY || _amp_stage == AMP_STAGE_RELEASE) {
			_advance_amp_stage();
		} else {
			_envelope_level = _amp_level;
		}
		return;
	}

	int samples_per_unit = _compute_amp_samples_per_unit(p_rate);
	if (samples_per_unit <= 0) {
		_amp_level = _amp_stage_target_level;
		_amp_stage_samples_left = 0;
		_amp_stage_increment = 0.0;
		if (_amp_stage == AMP_STAGE_ATTACK || _amp_stage == AMP_STAGE_DECAY || _amp_stage == AMP_STAGE_RELEASE) {
			_advance_amp_stage();
		} else {
			_envelope_level = _amp_level;
		}
		return;
	}

	double units = std::ceil(delta_abs * 128.0);
	if (units < 1.0) {
		units = 1.0;
	}
	_amp_stage_samples_left = (int)std::ceil(samples_per_unit * units);
	if (_amp_stage_samples_left <= 0) {
		_amp_stage_samples_left = 1;
	}
	_amp_stage_increment = delta / (double)_amp_stage_samples_left;
}

int SiOPMChannelMonolith::_compute_amp_samples_per_unit(int p_rate) const {
	int rate_index = CLAMP(p_rate, 0, 63);
	if (rate_index == 0) {
		return 0;
	}
	double base = 2.36514 * std::pow(2.0, 14.0 - (double)rate_index / 4.0) * 0.5;
	int samples = (int)(base + 0.5);
	return samples > 0 ? samples : 1;
}

void SiOPMChannelMonolith::_update_amp_envelope() {
	switch (_amp_stage) {
		case AMP_STAGE_ATTACK:
		case AMP_STAGE_DECAY:
		case AMP_STAGE_RELEASE: {
			if (_amp_stage_samples_left > 0) {
				_amp_level += _amp_stage_increment;
				_amp_stage_samples_left--;
				if (_amp_stage_samples_left <= 0) {
					_amp_level = _amp_stage_target_level;
					_advance_amp_stage();
				}
			} else {
				_amp_level = _amp_stage_target_level;
				_advance_amp_stage();
			}
		} break;
		case AMP_STAGE_SUSTAIN: {
			_amp_level = (double)_amp_sustain_level * 0.0078125;
		} break;
		case AMP_STAGE_IDLE:
		default:
			_amp_level = 0.0;
			break;
	}

	_envelope_level = CLAMP(_amp_level, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Grouped param setter
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::set_monolith_params(
		int p_sub_shape, int p_sub_level, int p_sub_drive, int p_pitch_drop,
		int p_osc1_shape, int p_osc2_shape,
		int p_mass, int p_bite, int p_shape,
		int p_drive_mode, int p_grind,
		int p_motion_target, int p_motion_amount, int p_motion_rate,
		int p_width, int p_low_lock, int p_lens, int p_glide,
		int p_sub_octave) {
	_sub_shape = CLAMP(p_sub_shape, 0, SUB_SHAPE_MAX - 1);
	_sub_level = CLAMP(p_sub_level, 0, 127);
	_sub_drive = CLAMP(p_sub_drive, 0, 127);
	_pitch_drop = CLAMP(p_pitch_drop, 0, 127);
	_sub_octave = CLAMP(p_sub_octave, 0, 2);
	_osc1_shape = CLAMP(p_osc1_shape, 0, OSC_SHAPE_MAX - 1);
	_osc2_shape = CLAMP(p_osc2_shape, 0, OSC_SHAPE_MAX - 1);
	_mass = CLAMP(p_mass, 0, 127);
	_bite = CLAMP(p_bite, 0, 127);
	_shape_warp = CLAMP(p_shape, 0, 127);
	_drive_mode = CLAMP(p_drive_mode, 0, DRIVE_MODE_MAX - 1);
	_grind = CLAMP(p_grind, 0, 127);
	_motion_target_param = CLAMP(p_motion_target, 0, MOTION_TARGET_MAX - 1);
	_motion_amount = CLAMP(p_motion_amount, 0, 127);
	_motion_rate = CLAMP(p_motion_rate, 0, 127);
	_width = CLAMP(p_width, 0, 127);
	_low_lock = CLAMP(p_low_lock, 0, 127);
	_lens = CLAMP(p_lens, 0, 127);
	_glide_time = CLAMP(p_glide, 0, 127);

	// Recompute glide coefficient.
	if (_glide_time > 0) {
		double glide_ms = 5.0 + (double)_glide_time * 15.0;
		double glide_samples = glide_ms * _sample_rate / 1000.0;
		_glide_coeff = 1.0 - std::exp(-4.0 / glide_samples);
	} else {
		_glide_coeff = 1.0;
	}

	// Motion LFO rate: 0 = ~0.1 Hz, 127 = ~20 Hz.
	double motion_freq = 0.1 * std::pow(200.0, (double)_motion_rate / 127.0);
	_motion_phase_inc = _freq_to_phase_inc(motion_freq);

	// Pitch-drop envelope decay: shorter pitch_drop = faster decay.
	if (_pitch_drop > 0) {
		double decay_ms = 20.0 + (double)_pitch_drop * 5.0;
		double decay_samples = decay_ms * _sample_rate / 1000.0;
		_pitch_env_decay_coeff = std::exp(-4.0 / decay_samples);
	}

	// --- Mass: thickness bundle ---
	double mass_n = (double)_mass / 127.0;
	// Detune: 0-50 cents range.
	double detune_cents = mass_n * 50.0;
	_mass_detune_pitch = detune_cents * 64.0 / 100.0;
	// Osc2 level boost: from 0.5 (equal mix) toward 1.0 at max mass.
	_mass_osc2_level = 0.5 + mass_n * 0.5;
	// Slow phase drift on osc2 for analog-like movement.
	double drift_hz = 0.05 + mass_n * 0.3;
	_mass_drift_inc = drift_hz / _sample_rate;
	// Drift depth scales with mass (0 = none, 1 = full bass-safe movement).
	_mass_drift_depth = mass_n;
	// Drive compensation: reduce input level as mass adds energy.
	_mass_drive_compensation = 1.0 / (1.0 + mass_n * 0.3);

	// --- Gain staging ---
	double grind_n = (double)_grind / 127.0;
	double bite_n = (double)_bite / 127.0;
	// Main harmonic layer level (not exposed, prevents domination over sub).
	_main_level = 0.7 - bite_n * 0.1;
	// Drive input trim: prevent loudness explosion as grind rises.
	_drive_input_trim = 1.0 / (1.0 + grind_n * 2.0);
	// Output makeup after drive compression.
	_drive_output_makeup = 1.0 / (1.0 + grind_n * 0.5);

	// --- Lens coefficients ---
	// LP split: cutoff sweeps 45-180 Hz based on lens amount.
	double lens_n = (double)_lens / 127.0;
	double lens_cutoff_hz = 45.0 + lens_n * 135.0;
	_lens_lp_coeff = 1.0 - std::exp(-TWO_PI * lens_cutoff_hz / _sample_rate);
	// HP on generated harmonics: ~200 Hz to remove mud.
	_lens_harm_hp_coeff = 1.0 - std::exp(-TWO_PI * 200.0 / _sample_rate);
}

// ---------------------------------------------------------------------------
// set_pitch override (for glide tracking)
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::set_pitch(int p_value) {
	_current_pitch = p_value;
	_target_pitch_f = (double)p_value;
	if (!_has_previous_note) {
		_glide_current_pitch = _target_pitch_f;
	}
}

// ---------------------------------------------------------------------------
// Channel params (shared LFO / filter / volume)
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
	p_params->set_operator_count(1);

	p_params->set_lfo_wave_shape(_lfo_wave_shape);
	p_params->set_lfo_time_mode(get_lfo_time_mode());
	switch (get_lfo_time_mode()) {
		case LFO_TIME_MODE_RATE:
			p_params->set_lfo_rate_value(_lfo_timer_step_buffer);
			break;
		case LFO_TIME_MODE_TIME:
			p_params->set_lfo_time_value(_lfo_timer_step_buffer);
			break;
		default:
			p_params->set_lfo_beat_value(_lfo_beat_division);
			break;
	}

	p_params->set_amplitude_modulation_depth(0);
	p_params->set_pitch_modulation_depth(0);

	p_params->set_amplitude_attack_rate(_amp_attack_rate);
	p_params->set_amplitude_decay_rate(_amp_decay_rate);
	p_params->set_amplitude_sustain_level(_amp_sustain_level);
	p_params->set_amplitude_release_rate(_amp_release_rate);

	for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
		p_params->set_master_volume(i, _volumes[i]);
	}
	p_params->set_instrument_gain_db(get_instrument_gain_db());
	p_params->set_pan(_pan);
}

void SiOPMChannelMonolith::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
	if (p_params->get_operator_count() == 0) {
		return;
	}

	if (p_with_modulation) {
		initialize_lfo(p_params->get_lfo_wave_shape());
		set_lfo_time_mode(p_params->get_lfo_time_mode());
		switch (p_params->get_lfo_time_mode()) {
			case LFO_TIME_MODE_RATE:
				set_lfo_frequency_step(p_params->get_lfo_rate_value());
				break;
			case LFO_TIME_MODE_TIME:
				set_lfo_frequency_step(p_params->get_lfo_time_value());
				break;
			default:
				set_lfo_frequency_step(p_params->get_lfo_beat_value());
				break;
		}

		set_amplitude_modulation(p_params->get_amplitude_modulation_depth());
		set_pitch_modulation(p_params->get_pitch_modulation_depth());
	}

	if (p_with_volume) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			_volumes.write[i] = p_params->get_master_volume(i);
		}

		_has_effect_send = false;
		for (int i = 1; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				_has_effect_send = true;
				break;
			}
		}

		_pan = p_params->get_pan();
	}
	set_instrument_gain_db(p_params->get_instrument_gain_db());

	_amp_attack_rate = CLAMP(p_params->get_amplitude_attack_rate(), 0, 63);
	_amp_decay_rate = CLAMP(p_params->get_amplitude_decay_rate(), 0, 63);
	_amp_sustain_level = CLAMP(p_params->get_amplitude_sustain_level(), 0, 128);
	set_release_rate(p_params->get_amplitude_release_rate());

	_filter_type = p_params->get_filter_type();
	set_sv_filter(
			p_params->get_filter_cutoff(),
			p_params->get_filter_resonance(),
			p_params->get_filter_attack_rate(),
			p_params->get_filter_decay_rate1(),
			p_params->get_filter_decay_rate2(),
			p_params->get_filter_release_rate(),
			p_params->get_filter_decay_offset1(),
			p_params->get_filter_decay_offset2(),
			p_params->get_filter_sustain_offset(),
			p_params->get_filter_release_offset());
}

// ---------------------------------------------------------------------------
// Note on / off / expression / rate overrides
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::offset_volume(int p_expression, int p_velocity) {
	_expression = (double)p_expression * 0.0078125;
}

void SiOPMChannelMonolith::set_all_attack_rate(int p_value) {
	_amp_attack_rate = CLAMP(p_value, 0, 63);
}

void SiOPMChannelMonolith::set_all_release_rate(int p_value) {
	set_release_rate(p_value);
}

void SiOPMChannelMonolith::set_release_rate(int p_value) {
	int clamped = CLAMP(p_value, 0, 63);
	if (_amp_release_rate == clamped) {
		return;
	}
	_amp_release_rate = clamped;
	if (_amp_stage == AMP_STAGE_RELEASE) {
		_configure_amp_stage(0.0, _amp_release_rate);
	}
}

void SiOPMChannelMonolith::note_on() {
	// Phase reset on note-on for tight transients.
	_sub_phase = 0;
	_sub_oct_phase = 0;
	_osc1_phase = 0;
	_osc2_phase = 0;

	// Pitch-drop envelope fires on note-on (any sub shape).
	if (_pitch_drop > 0) {
		_pitch_env_level = 1.0;
	}

	// Reset motion filter / drive-tone state for clean transients.
	_motion_filter_ic1 = 0.0;
	_motion_filter_ic2 = 0.0;
	_drive_tone_lp_z1 = 0.0;

	// Glide: keep previous pitch if legato, otherwise snap.
	if (_is_note_on && _glide_time > 0) {
		_has_previous_note = true;
	} else {
		_has_previous_note = false;
		_glide_current_pitch = _target_pitch_f;
	}

	_is_note_on = true;
	_is_idling = false;
	_declick_target = 1.0;

	_start_amp_envelope();

	SiOPMChannelBase::note_on();
}

void SiOPMChannelMonolith::note_off() {
	_is_note_on = false;
	if (_amp_stage != AMP_STAGE_IDLE) {
		_declick_target = 1.0;
	}

	_begin_amp_release();

	SiOPMChannelBase::note_off();
}

void SiOPMChannelMonolith::reset_channel_buffer_status() {
	SiOPMChannelBase::reset_channel_buffer_status();
	_is_idling = !_is_note_on && _amp_stage == AMP_STAGE_IDLE && _declick_level <= 0.0;
}

// ---------------------------------------------------------------------------
// Core DSP process
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::_process_monolith(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe = _out_pipe->get();

	// Pre-compute normalized control values.
	double sub_lvl = (double)_sub_level / 127.0;
	double sub_drv = (double)_sub_drive / 127.0;
	double bite_n = (double)_bite / 127.0;
	double warp_n = (double)_shape_warp / 127.0;
	double grind_n = (double)_grind / 127.0;
	double motion_n = (double)_motion_amount / 127.0;
	double low_lock_n = (double)_low_lock / 127.0;
	double lens_n = (double)_lens / 127.0;
	double width_n = (double)_width / 127.0;

	const double gain = _expression * PIPE_PEAK;

	for (int i = 0; i < p_length; i++) {
		// Declick ramp (secondary safety on top of amp envelope).
		if (_declick_level < _declick_target) {
			_declick_level = MIN(_declick_level + DECLICK_INCREMENT, _declick_target);
		} else if (_declick_level > _declick_target) {
			_declick_level = MAX(_declick_level - DECLICK_INCREMENT, 0.0);
			if (_declick_level <= 0.0 && _amp_stage == AMP_STAGE_IDLE) {
				_is_idling = true;
			}
		}

		// Amplitude envelope tick.
		_update_amp_envelope();

		// Motion LFO.
		_motion_phase += _motion_phase_inc;
		double motion_mod = 0.0;
		if (_motion_target_param != MOTION_OFF && motion_n > 0.001) {
			motion_mod = std::sin((double)_motion_phase * PHASE_TO_RAD) * motion_n;
		}

		// Mass drift: slow bipolar movement source for the main layer (never the
		// sub). Computed before phase increments so it can wobble osc2 detune/phase.
		_mass_drift_phase += _mass_drift_inc;
		if (_mass_drift_phase >= 1.0) {
			_mass_drift_phase -= 1.0;
		}
		_mass_drift_value = std::sin(_mass_drift_phase * TWO_PI) * _mass_drift_depth;

		// Update phase increments (glide + pitch-drop + mass drift happen here).
		_update_phase_increments(motion_mod);

		// Apply motion to target-specific modulation. Each target answers a
		// distinct sound-design question (see header enum notes):
		//   WARP      -> bends oscillator geometry (warp/fold/sync)
		//   OSC_SHAPE -> morphs the osc1<->osc2 source balance
		//   SUB_LEVEL -> ducks/pulses the sub
		// FILTER / RESONANCE / DRIVE_TONE are handled later in the signal chain.
		double warp_mod = warp_n;
		double sub_lvl_mod = sub_lvl;
		double osc_blend_mod = 0.0; // -1 favors osc1, +1 favors osc2
		switch (_motion_target_param) {
			case MOTION_WARP:
				warp_mod = CLAMP(warp_n + motion_mod * 0.5, 0.0, 1.0);
				break;
			case MOTION_OSC_SHAPE:
				osc_blend_mod = motion_mod;
				break;
			case MOTION_SUB_LEVEL:
				sub_lvl_mod = CLAMP(sub_lvl + motion_mod * 0.5, 0.0, 1.0);
				break;
			default:
				break;
		}

		// --- Sub oscillator ---
		double sub = _generate_sub_sample() * sub_lvl_mod;
		if (sub_drv > 0.001) {
			sub = _apply_drive(sub, DRIVE_WARM, sub_drv * 0.4);
		}

		// --- Main oscillators ---
		// Mass drift adds a tiny warp wobble for analog-like geometry movement.
		double osc_warp = CLAMP(warp_mod + _mass_drift_value * 0.03, 0.0, 1.0);

		double osc1 = _generate_osc_sample(_osc1_phase, _osc1_shape, osc_warp, _osc1_dt);
		double osc2 = _generate_osc_sample(_osc2_phase, _osc2_shape, osc_warp, _osc2_dt);

		// Width: frequency-aware phase offset on osc2 for thickening.
		// This is a timbre/thickness macro in v1; true stereo spread with
		// mono-low / wide-high crossover is deferred to v2 (requires
		// overriding buffer() for dual-pipe output).
		if (width_n > 0.01) {
			uint32_t offset = (uint32_t)(width_n * _osc2_phase_inc * 128.0);
			double osc2_w = _generate_osc_sample(_osc2_phase + offset, _osc2_shape, osc_warp, _osc2_dt);
			osc2 = osc2 * (1.0 - width_n * 0.5) + osc2_w * width_n * 0.5;
		}

		// Mix main oscillators. Mass drift wobbles the osc2 weight, and the
		// OSC_SHAPE motion target shifts the osc1<->osc2 source balance.
		double a1 = 1.0;
		double a2 = _mass_osc2_level * (1.0 + _mass_drift_value * 0.15);
		if (osc_blend_mod != 0.0) {
			double b = osc_blend_mod * 0.5; // +/-0.5 balance swing
			a1 *= (1.0 - b);
			a2 *= (1.0 + b);
		}
		double main_mix = (osc1 * a1 + osc2 * a2) / (a1 + a2);

		// Apply main level.
		main_mix *= _main_level;

		// Bite: harmonic emphasis via cubic waveshaping.
		if (bite_n > 0.01) {
			double saturated = fast_tanh(main_mix * (1.0 + bite_n * 4.0));
			main_mix = main_mix * (1.0 - bite_n) + saturated * bite_n;
		}

		// MOTION_DRIVE_TONE: animate distortion color via a pre-drive tilt EQ on
		// the main layer. Negative motion = darker/thicker low-mids into the
		// drive, positive = brighter/more teeth. The clean sub is untouched.
		if (_motion_target_param == MOTION_DRIVE_TONE && motion_n > 0.001) {
			_drive_tone_lp_z1 += _drive_tone_lp_coeff * (main_mix - _drive_tone_lp_z1);
			double low = _drive_tone_lp_z1;
			double high = main_mix - low;
			double g_low = CLAMP(1.0 - motion_mod, 0.0, 2.0);
			double g_high = CLAMP(1.0 + motion_mod, 0.0, 2.0);
			main_mix = low * g_low + high * g_high;
		}

		// --- Distortion chain (sub-protected) ---
		double sub_clean = sub * low_lock_n;
		double sub_dirty = sub * (1.0 - low_lock_n);

		if (grind_n > 0.001) {
			double drive_input = (main_mix + sub_dirty) * _drive_input_trim * _mass_drive_compensation;
			main_mix = _apply_drive(drive_input, _drive_mode, grind_n) * _drive_output_makeup;
		} else {
			main_mix = main_mix + sub_dirty;
		}

		// MOTION_FILTER / MOTION_RESONANCE: bass-safe internal SVF on the main
		// (dirty) layer only — the clean sub bypasses it so the low end stays
		// anchored even as cutoff/Q move. FILTER sweeps cutoff in octaves;
		// RESONANCE holds a fixed peak and animates Q (capped below self-osc).
		if ((_motion_target_param == MOTION_FILTER || _motion_target_param == MOTION_RESONANCE) && motion_n > 0.001) {
			double fc, q;
			if (_motion_target_param == MOTION_FILTER) {
				fc = MOTION_FILTER_BASE_HZ * std::pow(2.0, motion_mod * MOTION_FILTER_OCT);
				q = 1.0;
			} else {
				fc = MOTION_RES_BASE_HZ;
				q = CLAMP(MOTION_RES_BASE_Q + motion_mod * MOTION_RES_Q_DEPTH, 0.5, MOTION_RES_Q_MAX);
			}
			fc = CLAMP(fc, 30.0, _sample_rate * 0.45);

			// TPT state-variable filter (Zavalishin), lowpass tap.
			double g = std::tan(PI * fc / _sample_rate);
			double k = 1.0 / q;
			double a1f = 1.0 / (1.0 + g * (g + k));
			double a2f = g * a1f;
			double a3f = g * a2f;
			double v3 = main_mix - _motion_filter_ic2;
			double v1 = a1f * _motion_filter_ic1 + a2f * v3;
			double v2 = _motion_filter_ic2 + a2f * _motion_filter_ic1 + a3f * v3;
			_motion_filter_ic1 = 2.0 * v1 - _motion_filter_ic1;
			_motion_filter_ic2 = 2.0 * v2 - _motion_filter_ic2;
			main_mix = v2;
		}

		double output = main_mix + sub_clean;

		// --- Lens: harmonic enhancement for small speakers ---
		// Extracts low content via one-pole LP, generates harmonics via
		// half-wave rectification + saturation, then high-passes the
		// harmonics to avoid adding mud before mixing back.
		if (lens_n > 0.01) {
			_lens_lp_z1 += _lens_lp_coeff * (output - _lens_lp_z1);
			double lp = _lens_lp_z1;

			// Generate upper harmonics from low content.
			double harm = lp * lp * (lp > 0.0 ? 1.0 : -1.0);
			harm = fast_tanh(harm * 3.0);

			// HP the generated harmonics to remove mud (~200 Hz).
			double harm_hp = harm - _lens_harm_hp_z1;
			_lens_harm_hp_z1 += _lens_harm_hp_coeff * harm_hp;

			output += harm_hp * lens_n * 0.4;
		}

		// Safety clip (should only catch extremes with proper gain staging).
		output = CLAMP(output, -1.5, 1.5);
		output = fast_tanh(output);

		// Write to pipe with combined envelope + declick.
		double env = _envelope_level * _declick_level;
		int sample = (int)(output * gain * env);
		out_pipe->value = sample + base_pipe->value;

		// Advance phases.
		_sub_phase += _sub_phase_inc;
		_sub_oct_phase += _sub_phase_inc >> 1;
		_osc1_phase += _osc1_phase_inc;
		_osc2_phase += _osc2_phase_inc;

		// Pitch-drop envelope decay.
		if (_pitch_env_level > 0.001) {
			_pitch_env_level *= _pitch_env_decay_coeff;
		}

		in_pipe = in_pipe->next();
		base_pipe = base_pipe->next();
		out_pipe = out_pipe->next();
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

// ---------------------------------------------------------------------------
// Initialize / reset
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);

	_sample_rate = _table ? (double)_table->sampling_rate : 44100.0;

	_sub_shape = SUB_SINE;
	_sub_level = 80;
	_sub_drive = 0;
	_pitch_drop = 0;
	_sub_octave = 2;
	_osc1_shape = OSC_SAW;
	_osc2_shape = OSC_SAW;
	_mass = 40;
	_bite = 40;
	_shape_warp = 0;
	_drive_mode = DRIVE_WARM;
	_grind = 0;
	_motion_target_param = MOTION_OFF;
	_motion_amount = 0;
	_motion_rate = 40;
	_width = 0;
	_low_lock = 100;
	_lens = 0;
	_glide_time = 0;

	_current_pitch = 0;
	_expression = 1.0;
	_sub_phase = 0;
	_sub_oct_phase = 0;
	_osc1_phase = 0;
	_osc2_phase = 0;
	_noise_state = 0x12345678;
	_sub_phase_inc = 0;
	_osc1_phase_inc = 0;
	_osc2_phase_inc = 0;
	_osc1_dt = 0.0;
	_osc2_dt = 0.0;
	_pitch_env_level = 0.0;
	_motion_phase = 0;
	_motion_phase_inc = 0;
	_motion_value = 0.0;
	_glide_current_pitch = 0.0;
	_target_pitch_f = 0.0;
	_glide_coeff = 1.0;
	_has_previous_note = false;

	_amp_attack_rate = 0;
	_amp_decay_rate = 0;
	_amp_release_rate = 0;
	_amp_sustain_level = 128;
	_reset_amp_envelope();

	_declick_level = 0.0;
	_declick_target = 0.0;

	_mass_detune_pitch = 0.0;
	_mass_osc2_level = 0.5;
	_mass_drift_inc = 0.0;
	_mass_drift_phase = 0.0;
	_mass_drift_depth = 0.0;
	_mass_drift_value = 0.0;
	_mass_drive_compensation = 1.0;

	_motion_filter_ic1 = 0.0;
	_motion_filter_ic2 = 0.0;
	_drive_tone_lp_z1 = 0.0;
	_drive_tone_lp_coeff = 1.0 - std::exp(-TWO_PI * 700.0 / _sample_rate);

	_main_level = 0.7;
	_drive_input_trim = 1.0;
	_drive_output_makeup = 1.0;

	_lens_lp_z1 = 0.0;
	_lens_harm_hp_z1 = 0.0;
	_lens_lp_coeff = 1.0 - std::exp(-TWO_PI * 120.0 / _sample_rate);
	_lens_harm_hp_coeff = 1.0 - std::exp(-TWO_PI * 200.0 / _sample_rate);

	_process_function = Callable(this, "_process_monolith");
}

void SiOPMChannelMonolith::reset() {
	_sub_phase = 0;
	_sub_oct_phase = 0;
	_osc1_phase = 0;
	_osc2_phase = 0;
	_noise_state = 0x12345678;
	_pitch_env_level = 0.0;
	_motion_phase = 0;
	_mass_drift_phase = 0.0;
	_mass_drift_value = 0.0;

	_reset_amp_envelope();
	_declick_level = 0.0;
	_declick_target = 0.0;

	_motion_filter_ic1 = 0.0;
	_motion_filter_ic2 = 0.0;
	_drive_tone_lp_z1 = 0.0;

	_lens_lp_z1 = 0.0;
	_lens_harm_hp_z1 = 0.0;
	_has_previous_note = false;

	SiOPMChannelBase::reset();
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

String SiOPMChannelMonolith::_to_string() const {
	String params;
	params += "sub=" + itos(_sub_shape) + ":" + itos(_sub_level) + ", ";
	params += "osc=" + itos(_osc1_shape) + "/" + itos(_osc2_shape) + ", ";
	params += "mass=" + itos(_mass) + ", ";
	params += "bite=" + itos(_bite) + ", ";
	params += "grind=" + itos(_grind) + ":" + itos(_drive_mode) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64);
	return "SiOPMChannelMonolith: " + params;
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_process_monolith", "length"), &SiOPMChannelMonolith::_process_monolith);
}

SiOPMChannelMonolith::SiOPMChannelMonolith(SiOPMSoundChip *p_chip) :
		SiOPMChannelBase(p_chip) {
	_sample_rate = (_table ? (double)_table->sampling_rate : 44100.0);
	_process_function = Callable(this, "_process_monolith");
}
