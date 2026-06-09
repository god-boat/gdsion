/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_channel_ks.h"

#include <cmath>
#include "sion_enums.h"
#include "chip/channels/siopm_operator.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "chip/wave/siopm_wave_pcm_table.h"
#include "chip/wave/siopm_wave_table.h"
#include "chip/siopm_channel_params.h"
#include "sequencer/simml_ref_table.h"
#include "sequencer/simml_voice.h"

namespace {
static constexpr double KS_SUB_SAMPLE_SILENCE = 1.0;
static constexpr double KS_EXCITER_SCALE = 2048.0;
static constexpr int KS_COMB_BUFFER_SIZE = 256;
static constexpr double KS_PITCH_MOD_UNITS_PER_OCTAVE = 7680.0;
static constexpr double KS_BEND_RANGE = 320.0;
static constexpr int KS_REFERENCE_PITCH_INDEX = 60 << 6;
static constexpr double KS_PITCH_INDEX_TO_MOD_UNITS = 10.0;
}

// =============================================================================
// Body Resonator (biquad bandpass for synthetic body coloration)
// =============================================================================

void SiOPMChannelKS::BodyResonator::update_coeffs(double p_freq, double p_q, double p_gain, double p_sample_rate) {
	double w0 = 2.0 * Math_PI * p_freq / p_sample_rate;
	double cos_w0 = cos(w0);
	double sin_w0 = sin(w0);
	double alpha = sin_w0 / (2.0 * p_q);

	double a0 = 1.0 + alpha;
	b0 = (alpha) / a0;
	a1 = (-2.0 * cos_w0) / a0;
	a2 = (1.0 - alpha) / a0;
	gain = p_gain;
}

void SiOPMChannelKS::BodyResonator::init(double p_freq, double p_q, double p_gain, double p_sample_rate) {
	update_coeffs(p_freq, p_q, p_gain, p_sample_rate);
	z1 = 0;
	z2 = 0;
}

double SiOPMChannelKS::BodyResonator::process(double p_in) {
	double out = b0 * p_in - b0 * z2 - a1 * z1 - a2 * z2;
	z2 = z1;
	z1 = out;
	return out * gain;
}

void SiOPMChannelKS::BodyResonator::reset() {
	z1 = 0;
	z2 = 0;
}

// =============================================================================
// Exciter RNG
// =============================================================================

uint32_t SiOPMChannelKS::_exciter_rand() {
	_exciter_rng_state ^= _exciter_rng_state << 13;
	_exciter_rng_state ^= _exciter_rng_state >> 17;
	_exciter_rng_state ^= _exciter_rng_state << 5;
	return _exciter_rng_state;
}

double SiOPMChannelKS::_exciter_rand_bipolar() {
	return ((double)(_exciter_rand() & 0x7FFFFFFF) / (double)0x3FFFFFFF) - 1.0;
}

// =============================================================================
// Exciter: fills the delay buffer with shaped excitation signal
// =============================================================================

void SiOPMChannelKS::_fill_excitation(int *p_buffer, int p_length, double p_frequency) {
	if (p_length <= 0) {
		return;
	}

	double sample_rate = _table ? (double)_table->sampling_rate : 44100.0;
	int excite_samples = (int)(p_length * CLAMP(_exciter_length, 0.05, 1.0));
	if (excite_samples < 2) {
		excite_samples = 2;
	}
	if (excite_samples > p_length) {
		excite_samples = p_length;
	}

	double color_lpf = 0.1 + _exciter_color * 0.89;
	double drive_gain = 1.0 + _exciter_drive * 4.0;
	double shape_env_power = 0.5 + _exciter_shape * 3.5;
	double reference_frequency = _get_reference_exciter_frequency(sample_rate);
	double exciter_frequency = reference_frequency + (p_frequency - reference_frequency) * _exciter_pitch_follow;
	if (exciter_frequency < 1.0) {
		exciter_frequency = 1.0;
	}

	double lpf_state = 0.0;

	_exciter_rng_state = 12345 + (uint32_t)(_exciter_randomness * 99999.0);

	for (int i = 0; i < p_length; i++) {
		double env = 0.0;
		if (i < excite_samples) {
			double t = (double)i / (double)excite_samples;
			env = pow(1.0 - t, shape_env_power);
		}

		double raw = 0.0;

		switch (_exciter_type) {
			case EXCITER_NOISE: {
				raw = _exciter_rand_bipolar();
			} break;

			case EXCITER_PULSE: {
				double phase = fmod((double)i * exciter_frequency / sample_rate, 1.0);
				double duty = 0.1 + _exciter_shape * 0.4;
				raw = (phase < duty) ? 1.0 : -1.0;
			} break;

			case EXCITER_CLICK: {
				if (i < 3) {
					raw = (i == 0) ? 1.0 : -0.5;
				}
			} break;

			case EXCITER_FM: {
				double phase = (double)i * exciter_frequency / sample_rate;
				double mod_ratio = 1.0 + _exciter_color * 7.0;
				double mod_index = 2.0 + _exciter_drive * 8.0;
				raw = sin(2.0 * Math_PI * phase + mod_index * sin(2.0 * Math_PI * phase * mod_ratio));
			} break;

			case EXCITER_PCM: {
				double phase = (double)i * exciter_frequency / sample_rate;
				double saw = fmod(phase, 1.0) * 2.0 - 1.0;
				double sq = (fmod(phase, 1.0) < 0.5) ? 1.0 : -1.0;
				raw = saw * (1.0 - _exciter_color) + sq * _exciter_color;
			} break;

			case EXCITER_BURST: {
				double burst_env = (i < excite_samples) ? pow(1.0 - (double)i / excite_samples, 2.0) : 0.0;
				raw = _exciter_rand_bipolar() * burst_env;
				double burst_color = 0.3 + _exciter_color * 0.6;
				lpf_state += (raw - lpf_state) * burst_color;
				raw = lpf_state;
				lpf_state = 0.0;
			} break;

			case EXCITER_IMPULSE: {
				if (i == 0) {
					raw = 1.0;
				}
			} break;

			default:
				break;
		}

		if (_exciter_type != EXCITER_BURST) {
			lpf_state += (raw - lpf_state) * color_lpf;
			raw = lpf_state;
		}

		double randomized = raw;
		if (_exciter_randomness > 0.0) {
			randomized += _exciter_rand_bipolar() * _exciter_randomness * 0.3;
		}

		double driven = randomized * drive_gain;
		if (_exciter_drive > 0.5) {
			double soft_clip = tanh(driven * 1.5);
			driven = driven * (1.0 - _exciter_drive) + soft_clip * _exciter_drive;
		}

		double sample = driven * env * KS_EXCITER_SCALE;
		p_buffer[i] = (int)(p_buffer[i] * 0.3 + sample);
	}
}

// =============================================================================
// Loop filter configuration
// =============================================================================

void SiOPMChannelKS::_configure_loop_filter() {
	double brightness_norm = _loop_brightness;
	double tilt = _loop_tone_tilt;
	double mode_decay_lpf = _ks_decay_lpf;

	switch (_loop_filter_mode) {
		case LOOP_DARK: {
			mode_decay_lpf = 0.4 + (1.0 - _loop_damping) * 0.55;
			_loop_shelf_coef = 0.0;
			_loop_ap_coef = 0.0;
		} break;

		case LOOP_BRIGHT: {
			mode_decay_lpf = 0.7 + brightness_norm * 0.29;
			_loop_shelf_coef = 0.2 + brightness_norm * 0.6;
			_loop_ap_coef = 0.0;
		} break;

		case LOOP_BAND: {
			mode_decay_lpf = 0.5 + brightness_norm * 0.4;
			_loop_notch_freq = 0.2 + _loop_damping * 0.6;
			_loop_shelf_coef = 0.0;
			_loop_ap_coef = 0.0;
		} break;

		case LOOP_NOTCH: {
			mode_decay_lpf = 0.6 + brightness_norm * 0.35;
			_loop_notch_freq = 0.3 + tilt * 0.3;
			_loop_shelf_coef = 0.0;
			_loop_ap_coef = 0.0;
		} break;

		case LOOP_COMB: {
			mode_decay_lpf = 0.6 + brightness_norm * 0.35;
			_loop_comb_delay = 2 + (int)(_loop_damping * 12.0);
			if (_loop_comb_delay >= KS_COMB_BUFFER_SIZE) {
				_loop_comb_delay = KS_COMB_BUFFER_SIZE - 1;
			}
			_loop_shelf_coef = 0.0;
			_loop_ap_coef = 0.0;
		} break;

		case LOOP_METALLIC: {
			mode_decay_lpf = 0.55 + brightness_norm * 0.4;
			_loop_ap_coef = 0.3 + _loop_damping * 0.5;
			_loop_shelf_coef = tilt * 0.4;
		} break;

		case LOOP_DIFFUSED: {
			mode_decay_lpf = 0.5 + brightness_norm * 0.45;
			_loop_ap_coef = 0.5 + _loop_damping * 0.4;
			_loop_shelf_coef = 0.1;
		} break;

		default:
			break;
	}

	double tension_scale = 1.0 - CLAMP((double)_ks_tension, 0.0, 63.0) * 0.015625;
	double tension_mix = 0.35 + tension_scale * 0.65;
	_ks_decay_lpf = CLAMP(mode_decay_lpf * tension_mix, 0.01, 0.9999);

	_ks_decay = 1.0 - _loop_loss * 0.15;
	if (_ks_decay < 0.8) {
		_ks_decay = 0.8;
	}
	if (_ks_decay > 0.9999) {
		_ks_decay = 0.9999;
	}

	if (_is_note_held) {
		_decay_lpf = _ks_decay_lpf;
		_decay = _ks_decay;
	}
}

double SiOPMChannelKS::_apply_loop_filter_sample(double p_input) {
	_loop_lpf_z1 += (p_input - _loop_lpf_z1) * _decay_lpf;
	double out = _loop_lpf_z1;

	switch (_loop_filter_mode) {
		case LOOP_DARK:
			break;

		case LOOP_BRIGHT: {
			double high = p_input - _loop_shelf_z1;
			_loop_shelf_z1 = p_input;
			out += high * _loop_shelf_coef;
		} break;

		case LOOP_BAND: {
			double bp = p_input - _loop_notch_z2;
			_loop_notch_z2 = _loop_notch_z1;
			_loop_notch_z1 = p_input;
			out = out * (1.0 - _loop_notch_freq) + bp * _loop_notch_freq;
		} break;

		case LOOP_NOTCH: {
			double notch = p_input - 2.0 * _loop_notch_z1 + _loop_notch_z2;
			_loop_notch_z2 = _loop_notch_z1;
			_loop_notch_z1 = p_input;
			out -= notch * _loop_notch_freq * 0.5;
		} break;

		case LOOP_COMB: {
			int read_pos = _loop_comb_write_pos - _loop_comb_delay;
			if (read_pos < 0) {
				read_pos += KS_COMB_BUFFER_SIZE;
			}
			double delayed = 0.0;
			if (_loop_comb_buffer.size() > 0) {
				delayed = _loop_comb_buffer[read_pos];
			}
			double comb_mix = 0.3 + _loop_damping * 0.4;
			out = out * (1.0 - comb_mix) + delayed * comb_mix;
			if (_loop_comb_buffer.size() > 0) {
				_loop_comb_buffer.write[_loop_comb_write_pos] = out;
			}
			_loop_comb_write_pos = (_loop_comb_write_pos + 1) % KS_COMB_BUFFER_SIZE;
		} break;

		case LOOP_METALLIC: {
			double ap_in = out;
			double ap_out = -_loop_ap_coef * ap_in + _loop_ap_z1;
			_loop_ap_z1 = ap_in + _loop_ap_coef * ap_out;
			out = ap_out;
			if (_loop_shelf_coef != 0.0) {
				double high = out - _loop_shelf_z1;
				_loop_shelf_z1 = out;
				out += high * _loop_shelf_coef;
			}
		} break;

		case LOOP_DIFFUSED: {
			double ap_in = out;
			double ap_out = -_loop_ap_coef * ap_in + _loop_ap_z1;
			_loop_ap_z1 = ap_in + _loop_ap_coef * ap_out;
			out = ap_out;
		} break;

		default:
			break;
	}

	return out;
}

// =============================================================================
// Inharmonicity / stiffness
// =============================================================================

void SiOPMChannelKS::_configure_stiffness() {
	_allpass_coef = _stiffness * 0.7;
	_allpass2_coef = _dispersion * 0.5;
}

double SiOPMChannelKS::_apply_inharmonicity(double p_input) {
	if (_stiffness < 0.001 && _dispersion < 0.001) {
		return p_input;
	}

	double out = p_input;

	if (_allpass_coef > 0.0) {
		double ap_in = out;
		double ap_out = -_allpass_coef * ap_in + _allpass_z1;
		_allpass_z1 = ap_in + _allpass_coef * ap_out;
		out = ap_out;
	}

	if (_allpass2_coef > 0.0) {
		double ap2_in = out;
		double ap2_out = -_allpass2_coef * ap2_in + _allpass2_z1;
		_allpass2_z1 = ap2_in + _allpass2_coef * ap2_out;
		out = ap2_out;
	}

	if (_odd_even_balance != 0.5) {
		double balance = (_odd_even_balance - 0.5) * 2.0;
		double sign_component = (p_input > 0) ? 1.0 : -1.0;
		out = out * (1.0 - fabs(balance) * 0.3) + sign_component * fabs(out) * balance * 0.3;
	}

	return out;
}

// =============================================================================
// Body / cavity resonance
// =============================================================================

struct BodyPreset {
	double freq1, q1, gain1;
	double freq2, q2, gain2;
	double freq3, q3, gain3;
};

static const BodyPreset BODY_PRESETS[] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0 },              // NONE
	{ 120.0, 4.0, 3.0, 280.0, 6.0, 2.0, 520.0, 5.0, 1.5 },   // WOOD
	{ 800.0, 12.0, 2.5, 2200.0, 15.0, 2.0, 4500.0, 10.0, 1.0 }, // GLASS
	{ 350.0, 20.0, 3.5, 1100.0, 25.0, 3.0, 3300.0, 20.0, 2.0 }, // METAL
	{ 200.0, 3.0, 2.0, 600.0, 4.0, 1.5, 1200.0, 3.0, 1.0 },   // PLASTIC
	{ 150.0, 8.0, 3.0, 450.0, 10.0, 2.5, 900.0, 6.0, 1.5 },   // TUBE
	{ 90.0, 5.0, 3.5, 250.0, 7.0, 2.5, 500.0, 4.0, 1.5 },     // BOX
	{ 400.0, 3.0, 2.0, 1500.0, 4.0, 2.5, 5000.0, 3.0, 3.0 },  // TINY_SPEAKER
};

void SiOPMChannelKS::_configure_body_resonators(double p_sample_rate) {
	if (_body_type == BODY_NONE || _body_type >= BODY_MAX) {
		return;
	}

	const BodyPreset &preset = BODY_PRESETS[_body_type];
	double tune_mult = 0.5 + _body_tune * 1.5;
	double width_q_scale = 0.5 + (1.0 - _body_width) * 2.0;

	// Coefficients only -- never clear the biquad z-state here. This is called
	// from the live body setters, so wiping state on each edit would zipper the
	// ringing resonators. Fresh state is established by reset() at note_on.
	_body_resonators[0].update_coeffs(preset.freq1 * tune_mult, preset.q1 * width_q_scale, preset.gain1 * _body_amount, p_sample_rate);
	_body_resonators[1].update_coeffs(preset.freq2 * tune_mult, preset.q2 * width_q_scale, preset.gain2 * _body_amount, p_sample_rate);
	_body_resonators[2].update_coeffs(preset.freq3 * tune_mult, preset.q3 * width_q_scale, preset.gain3 * _body_amount, p_sample_rate);
}

double SiOPMChannelKS::_apply_body_resonance(double p_input) {
	if (_body_type == BODY_NONE || _body_amount < 0.001) {
		return p_input;
	}

	double body_signal = 0.0;
	for (int i = 0; i < BODY_RESONATOR_COUNT; i++) {
		body_signal += _body_resonators[i].process(p_input);
	}

	return p_input + body_signal;
}

// =============================================================================
// Pitch behavior
// =============================================================================

double SiOPMChannelKS::_get_effective_pitch_index(double p_pitch_index) const {
	double tracked_pitch = p_pitch_index;
	if (_pitch_keytrack != 1.0) {
		tracked_pitch = KS_REFERENCE_PITCH_INDEX + (tracked_pitch - KS_REFERENCE_PITCH_INDEX) * _pitch_keytrack;
	}
	if (_bend != 0.0) {
		tracked_pitch += _bend * KS_BEND_RANGE;
	}
	return tracked_pitch;
}

double SiOPMChannelKS::_get_pitch_wave_length(double p_pitch_index) const {
	if (_table == nullptr) {
		return 0.0;
	}

	const int pitch_idx_max = SiOPMRefTable::PITCH_TABLE_SIZE - 1;
	double clamped_pitch = CLAMP(p_pitch_index, 0.0, (double)pitch_idx_max);
	int pitch_index_a = (int)clamped_pitch;
	int pitch_index_b = MIN(pitch_index_a + 1, pitch_idx_max);
	double interp = clamped_pitch - (double)pitch_index_a;

	double wave_length_a = _table->pitch_wave_length[pitch_index_a];
	double wave_length_b = _table->pitch_wave_length[pitch_index_b];
	return wave_length_a + (wave_length_b - wave_length_a) * interp;
}

double SiOPMChannelKS::_get_reference_exciter_frequency(double p_sample_rate) const {
	double reference_wave_length = _get_pitch_wave_length((double)KS_REFERENCE_PITCH_INDEX);
	if (reference_wave_length <= 0.0) {
		return 261.6255653005986;
	}
	return p_sample_rate / reference_wave_length;
}

void SiOPMChannelKS::_update_pitch_modifiers(double &r_wave_length_mod) {
	double mod = 0.0;

	if (_pitch_drop > 0.0 && _pitch_drop_phase < 1.0) {
		double drop_amount = _pitch_drop * 200.0;
		mod += drop_amount * (1.0 - _pitch_drop_phase);
		_pitch_drop_phase += 0.002 + _pitch_drop * 0.01;
		if (_pitch_drop_phase > 1.0) {
			_pitch_drop_phase = 1.0;
		}
	}

	if (_pick_bend != 0.0 && _pick_bend_phase < 1.0) {
		double bend_amount = _pick_bend * 100.0;
		double curve = sin(_pick_bend_phase * Math_PI);
		mod += bend_amount * curve;
		_pick_bend_phase += 0.005;
		if (_pick_bend_phase > 1.0) {
			_pick_bend_phase = 1.0;
		}
	}

	if (_pitch_drift > 0.0) {
		_drift_phase += 0.0003 + _pitch_drift * 0.002;
		_drift_lfo = sin(_drift_phase * 2.0 * Math_PI) * _pitch_drift * 15.0;
		mod += _drift_lfo;
	}

	if (_tension_mod != 0.0) {
		double env_decay = (_decay < 0.99) ? (1.0 - _decay) * 20.0 : 0.0;
		mod += _tension_mod * env_decay * 30.0;
	}

	if (_is_gliding && _pitch_glide > 0.0) {
		_glide_current += (_glide_target - _glide_current) * _glide_rate;
		if (fabs(_glide_target - _glide_current) < 0.1) {
			_glide_current = _glide_target;
			_is_gliding = false;
		}
	}

	r_wave_length_mod = mod + (_glide_current - _glide_target);
}

// =============================================================================
// Loop filter mode setter
// =============================================================================

void SiOPMChannelKS::set_loop_filter_mode(LoopFilterMode p_mode) {
	// Live param: switching topology must not tear down the running loop state.
	// Every mode's z-state is zero-initialized at note_on and only ever holds
	// finite filter output, so any mode reads continuous, valid state. The shared
	// one-pole _loop_lpf_z1 stays continuous across all modes; the mode-specific
	// histories (notch/allpass/comb) are refreshed from the live signal within a
	// few samples. State is torn down only at the real boundaries -- note_on /
	// reset / initialize -- never on a parameter edit.
	_loop_filter_mode = p_mode;
	_configure_loop_filter();
}

// =============================================================================
// Body type/tune/width setters
// =============================================================================

void SiOPMChannelKS::set_body_type(BodyType p_type) {
	_body_type = p_type;
	double sample_rate = _table ? (double)_table->sampling_rate : 44100.0;
	_configure_body_resonators(sample_rate);
}

void SiOPMChannelKS::set_body_tune(double p_value) {
	_body_tune = CLAMP(p_value, 0.0, 1.0);
	double sample_rate = _table ? (double)_table->sampling_rate : 44100.0;
	_configure_body_resonators(sample_rate);
}

void SiOPMChannelKS::set_body_width(double p_value) {
	_body_width = CLAMP(p_value, 0.0, 1.0);
	double sample_rate = _table ? (double)_table->sampling_rate : 44100.0;
	_configure_body_resonators(sample_rate);
}

// =============================================================================
// Aggregate extended-param setter
// =============================================================================
//
// Converts the raw nominal ranges supplied by the voice/mailbox into the
// normalized values the typed setters expect. Keeping the conversion here (the
// same place the DSP "physics" live) mirrors how the other physical-model
// channels expose a single set_*_params() entry point and avoids duplicating
// the scaling at every call site.

void SiOPMChannelKS::set_ks_extended_params(
		int p_exciter_type, int p_exciter_color, int p_exciter_length,
		int p_exciter_shape, int p_exciter_drive, int p_exciter_pitch_follow, int p_exciter_randomness,
		int p_loop_filter_mode, int p_loop_damping, int p_loop_brightness,
		int p_loop_loss, int p_loop_tone_tilt,
		int p_stiffness, int p_dispersion, int p_bend, int p_odd_even,
		int p_body_type, int p_body_amount, int p_body_tune, int p_body_width,
		int p_pitch_drift, int p_pitch_drop, int p_pick_bend,
		int p_tension_mod, int p_keytrack, int p_glide,
		int p_release_mode) {
	set_exciter_type((ExciterType)p_exciter_type);
	set_exciter_color(p_exciter_color * 0.01);
	set_exciter_length(p_exciter_length * 0.01);
	set_exciter_shape(p_exciter_shape * 0.01);
	set_exciter_drive(p_exciter_drive * 0.01);
	set_exciter_pitch_follow(p_exciter_pitch_follow * 0.01);
	set_exciter_randomness(p_exciter_randomness * 0.01);
	set_loop_filter_mode((LoopFilterMode)p_loop_filter_mode);
	set_loop_damping(p_loop_damping * 0.01);
	set_loop_brightness(p_loop_brightness * 0.01);
	set_loop_loss(p_loop_loss * 0.01);
	set_loop_tone_tilt((p_loop_tone_tilt - 50) * 0.02);
	set_stiffness(p_stiffness * 0.01);
	set_dispersion(p_dispersion * 0.01);
	set_bend((p_bend - 50) * 0.02);
	set_odd_even_balance(p_odd_even * 0.01);
	set_body_type((BodyType)p_body_type);
	set_body_amount(p_body_amount * 0.01);
	set_body_tune(p_body_tune * 0.01);
	set_body_width(p_body_width * 0.01);
	set_pitch_drift(p_pitch_drift * 0.01);
	set_pitch_drop(p_pitch_drop * 0.01);
	set_pick_bend((p_pick_bend - 50) * 0.02);
	set_tension_mod((p_tension_mod - 50) * 0.02);
	set_pitch_keytrack(p_keytrack * 0.01);
	set_pitch_glide(p_glide * 0.01);
	set_release_mode((ReleaseMode)p_release_mode);
}

// =============================================================================
// Original API (preserved)
// =============================================================================

void SiOPMChannelKS::set_karplus_strong_params(int p_attack_rate, int p_decay_rate, int p_total_level, int p_fixed_pitch, int p_wave_shape, int p_tension) {
	int wave_shape = p_wave_shape;
	if (wave_shape == -1) {
		wave_shape = SiONPulseGeneratorType::PULSE_NOISE_PINK;
	}

	_ks_seed_type = KS_SEED_DEFAULT;

	set_algorithm(1, false, 0);
	set_feedback(0, 0);
	set_params_by_value(p_attack_rate, p_decay_rate, 0, 63, 15, p_total_level, 0, 0, 1, 0, 0, 0, 0, p_fixed_pitch);

	_active_operator->set_pulse_generator_type(wave_shape);
	Ref<SiOPMWaveTable> wave_table = _table->get_wave_table(_active_operator->get_pulse_generator_type());
	_active_operator->set_pitch_table_type(wave_table->get_default_pitch_table_type());

	set_all_release_rate(p_tension);
}

void SiOPMChannelKS::apply_ks_runtime_params(int p_attack_rate, int p_decay_rate, int p_total_level, int p_fixed_pitch, int p_wave_shape, int p_tension) {
	int wave_shape = (p_wave_shape == -1) ? SiONPulseGeneratorType::PULSE_NOISE_PINK : p_wave_shape;

	_active_operator->set_attack_rate(p_attack_rate);
	_active_operator->set_decay_rate(p_decay_rate > 48 ? 48 : p_decay_rate);
	_active_operator->set_total_level(p_total_level > 127 ? 127 : p_total_level);
	_active_operator->set_fixed_pitch_index(p_fixed_pitch << 6);
	_active_operator->set_pulse_generator_type(wave_shape);
	{
		Ref<SiOPMWaveTable> wave_table = _table->get_wave_table(_active_operator->get_pulse_generator_type());
		_active_operator->set_pitch_table_type(wave_table->get_default_pitch_table_type());
	}
	set_all_release_rate(p_tension);
}

void SiOPMChannelKS::apply_voice_params(const Ref<SiOPMChannelParams> &p_params, const Ref<SiOPMWaveBase> &p_wave_data, int p_tension) {
	if (p_params.is_null()) {
		return;
	}

	set_all_release_rate(p_tension);
	set_channel_params(p_params, false, true);

	if (p_wave_data.is_valid()) {
		set_wave_data(p_wave_data);
	}
}

void SiOPMChannelKS::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
	if (p_params->get_operator_count() == 0) {
		return;
	}

	_apply_common_channel_params(p_params, p_with_volume, p_with_modulation);

	Ref<SiOPMOperatorParams> op = p_params->get_operator_params(0);
	apply_ks_runtime_params(
			op->get_attack_rate(),
			op->get_decay_rate(),
			op->get_total_level(),
			op->get_fixed_pitch() >> 6,
			op->get_pulse_generator_type(),
			_ks_tension);
}

void SiOPMChannelKS::set_parameters(Vector<int> p_params) {
	_ks_seed_type = (p_params[0] == INT32_MIN ? KS_SEED_DEFAULT : (KSSeedType)p_params[0]);
	_ks_seed_index = (p_params[1] == INT32_MIN ? 0 : p_params[1]);

	switch (_ks_seed_type) {
		case KS_SEED_FM: {
			ERR_FAIL_INDEX(_ks_seed_index, SiMMLRefTable::VOICE_MAX);

			Ref<SiMMLVoice> voice = SiMMLRefTable::get_instance()->get_voice(_ks_seed_index);
			if (voice.is_valid()) {
				set_channel_params(voice->get_channel_params(), false);
			}
		} break;

		case KS_SEED_PCM: {
			ERR_FAIL_INDEX(_ks_seed_index, SiOPMRefTable::PCM_DATA_MAX);

			Ref<SiOPMWavePCMTable> pcm_table = _table->get_pcm_data(_ks_seed_index);
			if (pcm_table.is_valid()) {
				set_wave_data(pcm_table);
			}
		} break;

		default: {
			_ks_seed_type = KS_SEED_DEFAULT;
			set_params_by_value(p_params[1], p_params[2], 0, 63, 15, p_params[3], 0, 0, 1, 0, 0, 0, 0, p_params[4]);

			_active_operator->set_pulse_generator_type(p_params[5] == INT32_MIN ? SiONPulseGeneratorType::PULSE_NOISE_PINK : p_params[5]);
			Ref<SiOPMWaveTable> wave_table = _table->get_wave_table(_active_operator->get_pulse_generator_type());
			_active_operator->set_pitch_table_type(wave_table->get_default_pitch_table_type());
		} break;
	}
}

void SiOPMChannelKS::set_types(int p_pg_type, SiONPitchTableType p_pt_type) {
	_ks_seed_type = (KSSeedType)p_pg_type;
	_ks_seed_index = 0;
}

void SiOPMChannelKS::set_pitch(int p_value) {
	_previous_pitch_index = (double)_ks_pitch_index;
	_ks_pitch_index = p_value;
}

void SiOPMChannelKS::set_all_attack_rate(int p_value) {
	_operators[0]->set_attack_rate(p_value);
	_operators[0]->set_decay_rate(p_value > 48 ? 48 : p_value);
	_operators[0]->set_total_level(p_value > 48 ? 0 : (48 - p_value));
}

void SiOPMChannelKS::set_all_release_rate(int p_value) {
	_ks_tension = CLAMP(p_value, 0, 63);
	_configure_loop_filter();
}

void SiOPMChannelKS::set_release_rate(int p_value) {
	_ks_tension = CLAMP(p_value, 0, 63);
	_configure_loop_filter();
}

void SiOPMChannelKS::set_fixed_pitch(int p_value) {
	for (int i = 0; i < _operator_count; i++) {
		_operators[i]->set_fixed_pitch_index(p_value);
	}
}

// Volume control.

void SiOPMChannelKS::offset_volume(int p_expression, int p_velocity) {
	_expression = p_expression * 0.0078125;
	SiOPMChannelFM::offset_volume(128, p_velocity);
}

// LFO control.

void SiOPMChannelKS::_set_lfo_state(bool p_enabled) {
	_lfo_on = 0;
}

// =============================================================================
// Processing: note_on / note_off
// =============================================================================

void SiOPMChannelKS::note_on() {
	bool was_note_on = _is_note_on;
	_output = 0;
	_is_note_held = true;
	_bloom_timer = 0.0;
	_freeze_factor = 0.0;

	_pitch_drop_phase = 0.0;
	_pick_bend_phase = 0.0;
	_drift_phase = 0.0;
	_drift_lfo = 0.0;

	double target_pitch = _get_effective_pitch_index((double)_ks_pitch_index);
	if (_pitch_glide > 0.0 && was_note_on && _previous_pitch_index != (double)_ks_pitch_index) {
		_glide_current = _get_effective_pitch_index(_previous_pitch_index) * KS_PITCH_INDEX_TO_MOD_UNITS;
		_glide_target = target_pitch * KS_PITCH_INDEX_TO_MOD_UNITS;
		_is_gliding = true;
		_glide_rate = 0.001 + (1.0 - _pitch_glide) * 0.05;
	} else {
		_is_gliding = false;
		_glide_current = target_pitch * KS_PITCH_INDEX_TO_MOD_UNITS;
		_glide_target = target_pitch * KS_PITCH_INDEX_TO_MOD_UNITS;
	}

	_allpass_z1 = 0.0;
	_allpass2_z1 = 0.0;
	_loop_ap_z1 = 0.0;
	_loop_lpf_z1 = 0.0;
	_loop_shelf_z1 = 0.0;
	_loop_notch_z1 = 0.0;
	_loop_notch_z2 = 0.0;
	_loop_comb_z1 = 0.0;
	_loop_comb_write_pos = 0;
	if (_loop_comb_buffer.size() > 0) {
		_loop_comb_buffer.fill(0.0);
	}
	for (int i = 0; i < BODY_RESONATOR_COUNT; i++) {
		_body_resonators[i].reset();
	}

	const int delay_buffer_size = _ks_delay_buffer.size();
	if (delay_buffer_size > 0) {
		int *delay_buffer = _ks_delay_buffer.ptrw();

		double wave_length = _get_pitch_wave_length(target_pitch);
		if (wave_length < 2.0) {
			wave_length = 2.0;
		}
		int fill_length = (int)wave_length;
		if (fill_length > delay_buffer_size) {
			fill_length = delay_buffer_size;
		}
		if (fill_length < 2) {
			fill_length = 2;
		}

		double frequency = 44100.0 / wave_length;
		if (_table) {
			frequency = (double)_table->sampling_rate / wave_length;
		}

		_fill_excitation(delay_buffer, fill_length, frequency);
	}

	_configure_loop_filter();
	_configure_stiffness();
	_decay_lpf = _ks_decay_lpf;
	_decay = _ks_decay;

	SiOPMChannelFM::note_on();
}

void SiOPMChannelKS::note_off() {
	_is_note_held = false;

	switch (_release_mode) {
		case RELEASE_NATURAL: {
			_decay_lpf = _ks_decay_lpf * 0.8;
			_decay = _ks_decay * 0.995;
		} break;

		case RELEASE_PALM_MUTE: {
			_decay_lpf = 0.3;
			_decay = 0.85;
		} break;

		case RELEASE_CHOKE: {
			_decay_lpf = 0.1;
			_decay = 0.4;
		} break;

		case RELEASE_FREEZE: {
			_decay_lpf = _ks_decay_lpf;
			_decay = 0.9999;
			_freeze_factor = 1.0;
		} break;

		case RELEASE_BLOOM: {
			_bloom_timer = 1.0;
			_decay_lpf = MIN(_ks_decay_lpf + 0.2, 0.99);
			_decay = _ks_decay * 0.99;
		} break;

		default: {
			_decay_lpf = _ks_mute_decay_lpf;
			_decay = _ks_mute_decay;
		} break;
	}

	SiOPMChannelFM::note_off();
}

void SiOPMChannelKS::reset_channel_buffer_status() {
	SiOPMChannelFM::reset_channel_buffer_status();
	if (!_is_idling) {
		return;
	}

	if (Math::abs(_output) >= KS_SUB_SAMPLE_SILENCE) {
		_is_idling = false;
		return;
	}

	const int delay_buffer_size = _ks_delay_buffer.size();
	const int *delay_buffer = _ks_delay_buffer.ptr();
	for (int i = 0; i < delay_buffer_size; i++) {
		if (delay_buffer[i] != 0) {
			_is_idling = false;
			return;
		}
	}
}

// =============================================================================
// Core KS processing loop (with all new sections integrated)
// =============================================================================

void SiOPMChannelKS::_apply_karplus_strong(SinglyLinkedList<int>::Element *p_buffer_start, int p_length) {
	SinglyLinkedList<int>::Element *target = p_buffer_start;
	const int detune = _operators[0]->get_ptss_detune();
	const int delay_buffer_size = _ks_delay_buffer.size();
	if (delay_buffer_size <= 0) {
		return;
	}
	int *delay_buffer = _ks_delay_buffer.ptrw();

	double pitch_idx = _get_effective_pitch_index((double)(_ks_pitch_index + detune + _pitch_modulation_output_level));
	double wave_length_max = _get_pitch_wave_length(pitch_idx);
	if (wave_length_max < 2.0) {
		wave_length_max = 2.0;
	}

	double pitch_mod_accum = 0.0;

	for (int i = 0; i < p_length; i++) {
		// Update LFO.
		_lfo_timer -= _lfo_timer_step;
		if (_lfo_timer < 0) {
			_lfo_phase = (_lfo_phase + 1) & 255;

			int value_base = _lfo_wave_table[_lfo_phase];
			_pitch_modulation_output_level = (((value_base << 1) - 255) * _pitch_modulation_depth) >> 8;

			pitch_idx = _get_effective_pitch_index((double)(_ks_pitch_index + detune + _pitch_modulation_output_level));
			wave_length_max = _get_pitch_wave_length(pitch_idx);
			if (wave_length_max < 2.0) {
				wave_length_max = 2.0;
			}

			_lfo_timer += _lfo_timer_initial;
		}

		// Pitch behavior modifiers.
		_update_pitch_modifiers(pitch_mod_accum);
		double effective_wave_length = wave_length_max;
		if (pitch_mod_accum != 0.0) {
			double pitch_shift_ratio = pow(2.0, pitch_mod_accum / KS_PITCH_MOD_UNITS_PER_OCTAVE);
			effective_wave_length = wave_length_max / pitch_shift_ratio;
			if (effective_wave_length < 2.0) {
				effective_wave_length = 2.0;
			}
		}

		// Bloom fade-out after note-off.
		if (_bloom_timer > 0.0) {
			_bloom_timer -= 0.001;
			if (_bloom_timer <= 0.0) {
				_bloom_timer = 0.0;
				_decay_lpf = _ks_mute_decay_lpf;
				_decay = _ks_mute_decay;
			}
		}

		// Update KS delay read position.
		_ks_delay_buffer_index++;
		if (_ks_delay_buffer_index >= effective_wave_length) {
			_ks_delay_buffer_index = fmod(_ks_delay_buffer_index, effective_wave_length);
		}
		int buffer_index = (int)_ks_delay_buffer_index;
		if (buffer_index >= delay_buffer_size) {
			buffer_index %= delay_buffer_size;
		}

		// Read from delay buffer.
		double delayed_sample = (double)delay_buffer[buffer_index];

		// Apply inharmonicity (allpass in loop).
		delayed_sample = _apply_inharmonicity(delayed_sample);

		// Apply loop filter.
		double filtered = _apply_loop_filter_sample(delayed_sample);

		// Apply decay/loss.
		double decay_factor = _decay;
		if (_freeze_factor > 0.0) {
			decay_factor = 0.9999;
		}
		_output = filtered * decay_factor + target->value;

		if (Math::abs(_output) < KS_SUB_SAMPLE_SILENCE) {
			_output = 0;
		}

		delay_buffer[buffer_index] = (int)_output;

		// Apply body resonance post-loop.
		double body_output = _apply_body_resonance(_output);
		target->value = (int)body_output;

		target = target->next();
	}
}

// Buffer (same structure as before).
void SiOPMChannelKS::buffer(int p_length) {
	if (_is_idling) {
		buffer_no_process(p_length);
		return;
	}

	SinglyLinkedList<int>::Element *mono_out = _out_pipe->get();

	_process_function.call(p_length);

	if (_ring_pipe) {
		_apply_ring_modulation(mono_out, p_length);
	}

	_apply_karplus_strong(mono_out, p_length);

	if (_filter_on) {
		_apply_sv_filter(mono_out, p_length, _filter_variables);
	}
	if (_kill_fade_remaining_samples > 0) {
		_apply_kill_fade(mono_out, p_length);
	}

	if (_output_mode == OutputMode::OUTPUT_STANDARD && !_mute) {
		const bool is_redirected_main_stream = (_streams[0] != nullptr && _streams[0] != _sound_chip->get_output_stream());
		const double volume_coef = _expression * _instrument_gain;
		if (_has_effect_send) {
			for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
				if (_volumes[i] > 0) {
					SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
					if (stream) {
						const double volume = (i == 0 && is_redirected_main_stream) ? volume_coef : (_volumes[i] * volume_coef);
						const int pan = (i == 0 && is_redirected_main_stream) ? SiOPMStream::PAN_NONE : _pan;
						stream->write(mono_out, _buffer_index, p_length, volume, pan);
					}
				}
			}
		} else {
			SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
			const double volume = is_redirected_main_stream ? volume_coef : (_volumes[0] * volume_coef);
			const int pan = is_redirected_main_stream ? SiOPMStream::PAN_NONE : _pan;
			stream->write(mono_out, _buffer_index, p_length, volume, pan);
		}
	}

	_buffer_index += p_length;
}

// =============================================================================
// Initialize / Reset
// =============================================================================

void SiOPMChannelKS::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	_ks_delay_buffer_index = 0;
	_ks_pitch_index = 0;

	_ks_decay_lpf = 0.875;
	_ks_decay = 0.98;
	_ks_mute_decay_lpf = 0.5;
	_ks_mute_decay = 0.75;

	_output = 0;
	_decay_lpf = _ks_mute_decay_lpf;
	_decay = _ks_mute_decay;
	_expression = 1;

	// Exciter defaults.
	_exciter_type = EXCITER_NOISE;
	_exciter_color = 0.5;
	_exciter_length = 0.5;
	_exciter_shape = 0.5;
	_exciter_drive = 0.0;
	_exciter_pitch_follow = 1.0;
	_exciter_randomness = 0.0;
	_exciter_rng_state = 12345;

	// Loop filter defaults.
	_loop_filter_mode = LOOP_DARK;
	_loop_damping = 0.5;
	_loop_brightness = 0.5;
	_loop_loss = 0.02;
	_loop_tone_tilt = 0.0;
	_loop_ap_coef = 0.0;
	_loop_ap_z1 = 0.0;
	_loop_lpf_z1 = 0.0;
	_loop_shelf_coef = 0.0;
	_loop_shelf_z1 = 0.0;
	_loop_notch_freq = 0.5;
	_loop_notch_z1 = 0.0;
	_loop_notch_z2 = 0.0;
	_loop_comb_z1 = 0.0;
	_loop_comb_delay = 3;
	_loop_comb_write_pos = 0;
	if (_loop_comb_buffer.size() != KS_COMB_BUFFER_SIZE) {
		_loop_comb_buffer.resize(KS_COMB_BUFFER_SIZE);
	}
	_loop_comb_buffer.fill(0.0);

	// Inharmonicity defaults.
	_stiffness = 0.0;
	_dispersion = 0.0;
	_bend = 0.0;
	_odd_even_balance = 0.5;
	_allpass_coef = 0.0;
	_allpass_z1 = 0.0;
	_allpass2_coef = 0.0;
	_allpass2_z1 = 0.0;

	// Body defaults.
	_body_type = BODY_NONE;
	_body_amount = 0.0;
	_body_tune = 0.5;
	_body_width = 0.5;
	for (int i = 0; i < BODY_RESONATOR_COUNT; i++) {
		_body_resonators[i].reset();
	}

	// Pitch behavior defaults.
	_pitch_drift = 0.0;
	_pitch_drop = 0.0;
	_pick_bend = 0.0;
	_tension_mod = 0.0;
	_pitch_keytrack = 1.0;
	_pitch_glide = 0.0;
	_pitch_drop_phase = 0.0;
	_pick_bend_phase = 0.0;
	_drift_phase = 0.0;
	_drift_lfo = 0.0;
	_glide_current = 0.0;
	_glide_target = 0.0;
	_glide_rate = 0.0;
	_previous_pitch_index = 0.0;
	_is_gliding = false;

	// Release mode defaults.
	_release_mode = RELEASE_NATURAL;
	_is_note_held = false;
	_bloom_timer = 0.0;
	_freeze_factor = 0.0;

	SiOPMChannelFM::initialize(p_prev, p_buffer_index);

	_ks_seed_type = KS_SEED_DEFAULT;
	_ks_seed_index = 0;

	set_params_by_value(48, 48, 0, 63, 15, 0, 0, 0, 1, 0, 0, 0, -1, 0);
	_active_operator->set_pulse_generator_type(SiONPulseGeneratorType::PULSE_NOISE_PINK);
	_active_operator->set_pitch_table_type(SiONPitchTableType::PITCH_TABLE_PCM);

	if (_table) {
		int required = (int)Math::ceil(_table->pitch_wave_length[0]) + 16;
		if (required < 1) {
			required = 1;
		}
		if (_ks_delay_buffer.size() != required) {
			_ks_delay_buffer.resize_zeroed(required);
		}
	}

	_configure_loop_filter();
}

void SiOPMChannelKS::reset() {
	_ks_delay_buffer.fill(0);

	_loop_ap_z1 = 0.0;
	_loop_lpf_z1 = 0.0;
	_loop_shelf_z1 = 0.0;
	_loop_notch_z1 = 0.0;
	_loop_notch_z2 = 0.0;
	_loop_comb_z1 = 0.0;
	_loop_comb_write_pos = 0;
	if (_loop_comb_buffer.size() > 0) {
		_loop_comb_buffer.fill(0.0);
	}

	_allpass_z1 = 0.0;
	_allpass2_z1 = 0.0;
	_glide_current = 0.0;
	_glide_target = 0.0;
	_glide_rate = 0.0;
	_previous_pitch_index = 0.0;
	_is_gliding = false;

	for (int i = 0; i < BODY_RESONATOR_COUNT; i++) {
		_body_resonators[i].reset();
	}

	_bloom_timer = 0.0;
	_freeze_factor = 0.0;

	SiOPMChannelFM::reset();
}

String SiOPMChannelKS::_to_string() const {
	String params = "";

	params += "ops=" + itos(_operator_count) + ", ";
	params += "exciter=" + itos((int)_exciter_type) + ", ";
	params += "loop=" + itos((int)_loop_filter_mode) + ", ";
	params += "stiff=" + rtos(_stiffness) + ", ";
	params += "body=" + itos((int)_body_type) + ", ";
	params += "release=" + itos((int)_release_mode) + ", ";
	params += "feedback=" + itos(_input_level - 6) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64) + "";

	return "SiOPMChannelKS: " + params;
}

SiOPMChannelKS::SiOPMChannelKS(SiOPMSoundChip *p_chip) : SiOPMChannelFM(p_chip) {
	_ks_delay_buffer.resize_zeroed(KS_BUFFER_SIZE);
	_loop_comb_buffer.resize(KS_COMB_BUFFER_SIZE);
	_loop_comb_buffer.fill(0.0);
}
