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
static constexpr double PHASE_TO_RAD = TWO_PI / 4294967296.0;
static constexpr double PHASE_TO_NORM = 1.0 / 4294967296.0;
static constexpr double PIPE_PEAK = 8192.0;

// Fast tanh approximation (Pade 3/3).
static inline double fast_tanh(double x) {
	if (x < -3.0) return -1.0;
	if (x > 3.0) return 1.0;
	double x2 = x * x;
	return x * (27.0 + x2) / (27.0 + 9.0 * x2);
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

	// Sub pitch with optional 808 pitch drop.
	double sub_pitch = pitch;
	if (_sub_shape == SUB_808 && _pitch_env_level > 0.001) {
		sub_pitch += _pitch_env_level * (double)_pitch_drop * 0.5;
	}
	_sub_phase_inc = _pitch_to_phase_inc(sub_pitch);

	// Main oscillators with mass-based detune.
	double detune_cents = (double)_mass * 0.25;
	double detune_pitch = detune_cents * 64.0 / 100.0;

	double phase_mod = 0.0;
	if (_motion_target_param == MOTION_PHASE) {
		phase_mod = p_motion_mod;
	}
	(void)phase_mod;

	_osc1_phase_inc = _pitch_to_phase_inc(pitch - detune_pitch * 0.5);
	_osc2_phase_inc = _pitch_to_phase_inc(pitch + detune_pitch * 0.5);
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
			double sq = (t < 0.5) ? 1.0 : -1.0;
			sample = fast_tanh(sq * 3.0);
		} break;

		case SUB_SATURATED_SINE: {
			double s = std::sin((double)_sub_phase * PHASE_TO_RAD);
			sample = fast_tanh(s * 2.0);
		} break;

		case SUB_OCTAVE_STACK: {
			double fund = std::sin((double)_sub_phase * PHASE_TO_RAD);
			double oct_below = std::sin((double)(_sub_phase >> 1) * PHASE_TO_RAD);
			sample = fund * 0.6 + oct_below * 0.4;
		} break;

		case SUB_CLICK: {
			double s = std::sin((double)_sub_phase * PHASE_TO_RAD);
			sample = s + _click_level * 0.5;
		} break;

		case SUB_808: {
			sample = std::sin((double)_sub_phase * PHASE_TO_RAD);
		} break;

		default:
			sample = std::sin((double)_sub_phase * PHASE_TO_RAD);
			break;
	}

	return sample;
}

// ---------------------------------------------------------------------------
// Main oscillator (per voice)
// ---------------------------------------------------------------------------

double SiOPMChannelMonolith::_generate_osc_sample(uint32_t p_phase, int p_shape, double p_warp) {
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
		} break;

		case OSC_PULSE: {
			// Pulse with warp controlling duty cycle (0.1 to 0.9).
			double duty = 0.5 + p_warp * 0.35;
			sample = (t < duty) ? 1.0 : -1.0;
			// Soften edges slightly to reduce aliasing.
			double edge_width = 0.01;
			if (t < edge_width) {
				sample *= t / edge_width;
			} else if (t > duty - edge_width && t < duty + edge_width) {
				double d = (t - duty) / edge_width;
				sample = 1.0 - 2.0 * CLAMP((d + 1.0) * 0.5, 0.0, 1.0);
			}
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
		} break;

		case OSC_SINE_FOLD: {
			// Sine through wavefolder. Warp controls fold intensity.
			double s = std::sin((double)p_phase * PHASE_TO_RAD);
			double fold = 1.0 + p_warp * 6.0;
			sample = std::sin(s * fold * 1.5707963);
		} break;

		case OSC_FORMANT: {
			// Two-frequency formant. Warp sweeps formant position.
			double carrier = std::sin((double)p_phase * PHASE_TO_RAD);
			double formant_ratio = 2.0 + p_warp * 6.0;
			double formant = std::sin((double)p_phase * PHASE_TO_RAD * formant_ratio);
			// Window the formant by the carrier amplitude envelope.
			double window = 0.5 + 0.5 * std::cos((double)p_phase * PHASE_TO_RAD);
			sample = carrier * 0.5 + formant * window * 0.5;
		} break;

		case OSC_SYNC: {
			// Hard sync emulation. Warp controls sync ratio.
			double sync_ratio = 1.0 + p_warp * 4.0;
			double sync_phase = std::fmod(t * sync_ratio, 1.0);
			sample = 2.0 * sync_phase - 1.0;
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
	return std::sin(x * 1.5707963);
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
// Grouped param setter
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::set_monolith_params(
		int p_sub_shape, int p_sub_level, int p_sub_drive, int p_pitch_drop,
		int p_osc1_shape, int p_osc2_shape,
		int p_mass, int p_bite, int p_shape,
		int p_drive_mode, int p_grind,
		int p_motion_target, int p_motion_amount, int p_motion_rate,
		int p_width, int p_low_lock, int p_lens, int p_glide) {
	_sub_shape = CLAMP(p_sub_shape, 0, SUB_SHAPE_MAX - 1);
	_sub_level = CLAMP(p_sub_level, 0, 127);
	_sub_drive = CLAMP(p_sub_drive, 0, 127);
	_pitch_drop = CLAMP(p_pitch_drop, 0, 127);
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

	// 808 pitch envelope decay: shorter pitch_drop = faster decay.
	if (_pitch_drop > 0) {
		double decay_ms = 20.0 + (double)_pitch_drop * 5.0;
		double decay_samples = decay_ms * _sample_rate / 1000.0;
		_pitch_env_decay_coeff = std::exp(-4.0 / decay_samples);
	}
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
// Note on / off / expression
// ---------------------------------------------------------------------------

void SiOPMChannelMonolith::offset_volume(int p_expression, int p_velocity) {
	_expression = (double)p_expression * 0.0078125;
}

void SiOPMChannelMonolith::note_on() {
	// Phase reset on note-on for tight transients.
	_sub_phase = 0;
	_osc1_phase = 0;
	_osc2_phase = 0;

	// 808 pitch envelope fires on note-on.
	if (_sub_shape == SUB_808 && _pitch_drop > 0) {
		_pitch_env_level = 1.0;
	}

	// Click transient.
	if (_sub_shape == SUB_CLICK) {
		_click_level = 1.0;
	}

	// Glide: keep previous pitch if legato, otherwise snap.
	if (_is_note_on && _glide_time > 0) {
		// Legato: glide from current pitch to new target.
		_has_previous_note = true;
	} else {
		_has_previous_note = false;
		_glide_current_pitch = _target_pitch_f;
	}

	_is_note_on = true;
	_is_idling = false;
	_declick_target = 1.0;

	SiOPMChannelBase::note_on();
}

void SiOPMChannelMonolith::note_off() {
	_is_note_on = false;
	_declick_target = 0.0;

	SiOPMChannelBase::note_off();
}

void SiOPMChannelMonolith::reset_channel_buffer_status() {
	SiOPMChannelBase::reset_channel_buffer_status();
	_is_idling = !_is_note_on && _declick_level <= 0.0;
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
		// Declick ramp.
		if (_declick_level < _declick_target) {
			_declick_level = MIN(_declick_level + DECLICK_INCREMENT, _declick_target);
		} else if (_declick_level > _declick_target) {
			_declick_level = MAX(_declick_level - DECLICK_INCREMENT, 0.0);
			if (_declick_level <= 0.0) {
				_is_idling = true;
			}
		}

		// Motion LFO.
		_motion_phase += _motion_phase_inc;
		double motion_mod = 0.0;
		if (_motion_target_param != MOTION_OFF && motion_n > 0.001) {
			motion_mod = std::sin((double)_motion_phase * PHASE_TO_RAD) * motion_n;
		}

		// Update phase increments (glide + 808 pitch env happen here).
		_update_phase_increments(motion_mod);

		// Apply motion to target-specific modulation.
		double warp_mod = warp_n;
		double sub_lvl_mod = sub_lvl;
		if (_motion_target_param == MOTION_WARP) {
			warp_mod = CLAMP(warp_n + motion_mod * 0.5, 0.0, 1.0);
		} else if (_motion_target_param == MOTION_SUB_LEVEL) {
			sub_lvl_mod = CLAMP(sub_lvl + motion_mod * 0.5, 0.0, 1.0);
		}

		// --- Sub oscillator ---
		double sub = _generate_sub_sample() * sub_lvl_mod;
		if (sub_drv > 0.001) {
			sub = _apply_drive(sub, DRIVE_WARM, sub_drv * 0.4);
		}

		// --- Main oscillators ---
		double osc_warp = warp_mod;
		if (_motion_target_param == MOTION_OSC_SHAPE) {
			osc_warp = CLAMP(warp_mod + motion_mod * 0.4, 0.0, 1.0);
		}

		double osc1 = _generate_osc_sample(_osc1_phase, _osc1_shape, osc_warp);
		double osc2 = _generate_osc_sample(_osc2_phase, _osc2_shape, osc_warp);

		// Width: phase offset on osc2 for stereo-like thickening.
		if (width_n > 0.01) {
			uint32_t offset = (uint32_t)(width_n * 1073741824.0);
			double osc2_w = _generate_osc_sample(_osc2_phase + offset, _osc2_shape, osc_warp);
			osc2 = osc2 * (1.0 - width_n * 0.5) + osc2_w * width_n * 0.5;
		}

		// Mix main oscillators.
		double main_mix = (osc1 + osc2) * 0.5;

		// Bite: harmonic emphasis via cubic waveshaping.
		if (bite_n > 0.01) {
			double saturated = fast_tanh(main_mix * (1.0 + bite_n * 4.0));
			main_mix = main_mix * (1.0 - bite_n) + saturated * bite_n;
		}

		// --- Distortion chain (sub-protected) ---
		double sub_clean = sub * low_lock_n;
		double sub_dirty = sub * (1.0 - low_lock_n);

		if (grind_n > 0.001) {
			main_mix = _apply_drive(main_mix + sub_dirty, _drive_mode, grind_n);
		} else {
			main_mix = main_mix + sub_dirty;
		}

		double output = main_mix + sub_clean;

		// --- Lens: harmonic enhancement for small speakers ---
		if (lens_n > 0.01) {
			// High-pass to isolate upper content, then generate harmonics
			// from the low-frequency content and mix them in.
			double hp = output - _lens_hp_z1;
			// ~80 Hz cutoff at 44.1 kHz (coefficient ≈ 1 - 80*2π/sr).
			_lens_hp_z1 += hp * (500.0 / _sample_rate);
			double lp = _lens_hp_z1;
			// Generate upper harmonics from the low-frequency content via
			// half-wave rectification + saturation.
			double harm = lp * lp * (lp > 0.0 ? 1.0 : -1.0);
			harm = fast_tanh(harm * 3.0);
			output += harm * lens_n * 0.4;

			// Compensate sub level reduction at high lens values.
			output *= 1.0 + lens_n * 0.15;
		}

		// Safety clip.
		output = CLAMP(output, -1.2, 1.2);
		output = fast_tanh(output);

		// Write to pipe.
		int sample = (int)(output * gain * _declick_level);
		out_pipe->value = sample + base_pipe->value;

		// Advance phases.
		_sub_phase += _sub_phase_inc;
		_osc1_phase += _osc1_phase_inc;
		_osc2_phase += _osc2_phase_inc;

		// 808 pitch envelope decay.
		if (_sub_shape == SUB_808 && _pitch_env_level > 0.001) {
			_pitch_env_level *= _pitch_env_decay_coeff;
		}

		// Click transient decay.
		if (_click_level > 0.001) {
			_click_level *= CLICK_DECAY;
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
	_osc1_phase = 0;
	_osc2_phase = 0;
	_noise_state = 0x12345678;
	_sub_phase_inc = 0;
	_osc1_phase_inc = 0;
	_osc2_phase_inc = 0;
	_pitch_env_level = 0.0;
	_click_level = 0.0;
	_motion_phase = 0;
	_motion_phase_inc = 0;
	_motion_value = 0.0;
	_glide_current_pitch = 0.0;
	_target_pitch_f = 0.0;
	_glide_coeff = 1.0;
	_has_previous_note = false;
	_declick_level = 0.0;
	_declick_target = 0.0;
	_lens_hp_z1 = 0.0;

	_process_function = Callable(this, "_process_monolith");
}

void SiOPMChannelMonolith::reset() {
	_sub_phase = 0;
	_osc1_phase = 0;
	_osc2_phase = 0;
	_noise_state = 0x12345678;
	_pitch_env_level = 0.0;
	_click_level = 0.0;
	_motion_phase = 0;
	_declick_level = 0.0;
	_declick_target = 0.0;
	_lens_hp_z1 = 0.0;
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
