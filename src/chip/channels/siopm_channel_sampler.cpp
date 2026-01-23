/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "siopm_channel_sampler.h"

#include "sion_enums.h"
#include "chip/siopm_channel_params.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "chip/wave/siopm_wave_base.h"
#include "chip/wave/siopm_wave_sampler_data.h"
#include "chip/wave/siopm_wave_sampler_table.h"
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>

// Convert AM delta (log-table index domain) into a linear gain multiplier.
// Adding delta to the log index multiplies the linear amplitude by 2^(-delta / 512).
static _FORCE_INLINE_ double _am_gain_from_log_delta(int p_delta) {
	return std::pow(2.0, -(double)p_delta / 512.0);
}

void SiOPMChannelSampler::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
	p_params->set_operator_count(1);

	p_params->set_envelope_frequency_ratio(_frequency_ratio);

	p_params->set_lfo_wave_shape(_lfo_wave_shape);
	p_params->set_lfo_frequency_step(_lfo_timer_step_buffer);
	p_params->set_lfo_time_mode(get_lfo_time_mode());

	p_params->set_amplitude_modulation_depth(_amplitude_modulation_depth);
	p_params->set_pitch_modulation_depth(_pitch_modulation_depth);

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

void SiOPMChannelSampler::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
	if (p_params->get_operator_count() == 0) {
		return;
	}

	// Frequency/LFO/modulation (parity with PCM).
	set_frequency_ratio(p_params->get_envelope_frequency_ratio());
	if (p_with_modulation) {
		initialize_lfo(p_params->get_lfo_wave_shape());
		set_lfo_time_mode(p_params->get_lfo_time_mode());
		set_lfo_frequency_step(p_params->get_lfo_frequency_step());

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

	// Filter.
	_filter_type = p_params->get_filter_type();
	{
		int filter_cutoff = p_params->get_filter_cutoff();
		int filter_resonance = p_params->get_filter_resonance();
		int filter_ar = p_params->get_filter_attack_rate();
		int filter_dr1 = p_params->get_filter_decay_rate1();
		int filter_dr2 = p_params->get_filter_decay_rate2();
		int filter_rr = p_params->get_filter_release_rate();
		int filter_dc1 = p_params->get_filter_decay_offset1();
		int filter_dc2 = p_params->get_filter_decay_offset2();
		int filter_sc = p_params->get_filter_sustain_offset();
		int filter_rc = p_params->get_filter_release_offset();
		set_sv_filter(filter_cutoff, filter_resonance, filter_ar, filter_dr1, filter_dr2, filter_rr, filter_dc1, filter_dc2, filter_sc, filter_rc);
	}

	set_amp_attack_rate(p_params->get_amplitude_attack_rate());
	set_amp_decay_rate(p_params->get_amplitude_decay_rate());
	set_amp_sustain_level(p_params->get_amplitude_sustain_level());
	set_amp_release_rate(p_params->get_amplitude_release_rate());
}

void SiOPMChannelSampler::set_wave_data(const Ref<SiOPMWaveBase> &p_wave_data) {
	_sampler_table = p_wave_data;
	// NOTE: Don't set _sample_data here - it should only hold individual samples from get_sample(),
	// not the table itself. This is critical for voice stealing declick to work correctly.
}

void SiOPMChannelSampler::set_types(int p_pg_type, SiONPitchTableType p_pt_type) {
	_bank_number = p_pg_type & 3;
}

int SiOPMChannelSampler::get_pitch() const {
	return (_wave_number << 6) + _fine_pitch;
}

void SiOPMChannelSampler::set_pitch(int p_value) {
	_wave_number = p_value >> 6;
	_fine_pitch = p_value & 0x3F; // lower 6 bits

	double delta_semitones = 0.0;
	if (_sample_data.is_valid() && _sample_data->is_fixed_pitch() && _has_note_on_pitch) {
		// For fixed-pitch samples, preserve envelope deltas but ignore base note transposition.
		delta_semitones = ((double)(p_value - _note_on_pitch)) / 64.0;
	} else {
		// Calculate pitch ratio relative to middle C (note 60).
		delta_semitones = (double)(_wave_number - 60) + ((double)_fine_pitch / 64.0); // 64 fine steps per semitone
	}

	double user_offset = 0.0;
	if (_sample_data.is_valid()) {
		user_offset = (double)_sample_data->get_root_offset()
			+ (double)_sample_data->get_coarse_offset()
			+ ((double)_sample_data->get_fine_offset() / 100.0);
	}

	double total_pitch = delta_semitones + user_offset;
	_pitch_step = std::pow(2.0, total_pitch / 12.0);
}

void SiOPMChannelSampler::set_phase(int p_value) {
	_sample_start_phase = p_value;
}

// Volume control.

void SiOPMChannelSampler::offset_volume(int p_expression, int p_velocity) {
	_expression = p_expression * p_velocity * 0.00006103515625; // 1/16384
}

// LFO control.

void SiOPMChannelSampler::set_frequency_ratio(int p_ratio) {
	_frequency_ratio = p_ratio;

	double value_coef = (p_ratio != 0) ? (100.0 / p_ratio) : 1.0;
	_lfo_timer_initial = (int)(SiOPMRefTable::LFO_TIMER_INITIAL * value_coef);
	_amp_rate_scale = value_coef;
	_refresh_active_amp_stage();
}

void SiOPMChannelSampler::initialize_lfo(int p_waveform, Vector<int> p_custom_wave_table) {
	SiOPMChannelBase::initialize_lfo(p_waveform, p_custom_wave_table);

	_set_lfo_state(false);

	_amplitude_modulation_depth = 0;
	_pitch_modulation_depth = 0;
	_amplitude_modulation_output_level = 0;
	_pitch_modulation_output_level = 0;
	_amplitude_modulation_gain = 1.0;
}

void SiOPMChannelSampler::set_amplitude_modulation(int p_depth) {
	_amplitude_modulation_depth = p_depth << 2;
	_amplitude_modulation_output_level = (_lfo_wave_table[_lfo_phase] * _amplitude_modulation_depth) >> 7 << 3;

	_set_lfo_state(_pitch_modulation_depth != 0 || _amplitude_modulation_depth != 0);
	// Recompute linear gain from AM delta for parity with FM's log-domain AM.
	_amplitude_modulation_gain = _am_gain_from_log_delta(_amplitude_modulation_output_level);
}

void SiOPMChannelSampler::set_pitch_modulation(int p_depth) {
	_pitch_modulation_depth = p_depth;
	_pitch_modulation_output_level = (((_lfo_wave_table[_lfo_phase] << 1) - 255) * _pitch_modulation_depth) >> 8;

	_set_lfo_state(_pitch_modulation_depth != 0 || _amplitude_modulation_depth != 0);
}

void SiOPMChannelSampler::_set_lfo_state(bool p_enabled) {
	_lfo_on = (int)p_enabled;
	_lfo_timer_step = p_enabled ? _lfo_timer_step_buffer : 0;
}

void SiOPMChannelSampler::_set_lfo_timer(int p_value) {
	_lfo_timer = (p_value > 0 ? 1 : 0);
	_lfo_timer_step = p_value;
	_lfo_timer_step_buffer = p_value;
}

void SiOPMChannelSampler::_update_lfo() {
	if (_lfo_on == 0) {
		return;
	}
	_lfo_timer -= _lfo_timer_step;
	if (_lfo_timer >= 0) {
		return;
	}

	_lfo_phase = (_lfo_phase + 1) & 255;

	int value_base = _lfo_wave_table[_lfo_phase];
	_amplitude_modulation_output_level = (value_base * _amplitude_modulation_depth) >> 7 << 3;
	_pitch_modulation_output_level = (((value_base << 1) - 255) * _pitch_modulation_depth) >> 8;

	_lfo_timer += _lfo_timer_initial;
	// Update cached linear AM gain from log delta.
	_amplitude_modulation_gain = _am_gain_from_log_delta(_amplitude_modulation_output_level);
}

// Processing.

void SiOPMChannelSampler::note_on() {
	if (_wave_number < 0) {
		return;
	}

	// Voice-stealing declick: defer note_on if we're currently playing audible audio.
	// Check both envelope state and sample data validity to avoid clicks.
	const bool envelope_audible = (_amp_stage != AMP_STAGE_IDLE && _amp_level > 0.1);
	const bool treat_as_voice_steal = envelope_audible && _sample_data.is_valid();

	if (treat_as_voice_steal) {
		// Store the new note parameters for later execution.
		_has_deferred_note_on = true;
		_deferred_wave_number = _wave_number;
		_deferred_sample_start_phase = _sample_start_phase;
		_deferred_pitch_step = _pitch_step;

		// Trigger fast release to fade out current sample.
		if (_amp_stage != AMP_STAGE_RELEASE) {
			_begin_amp_release();
		}
		_configure_amp_stage(0.0, 63); // Rate 63 = fastest
		return;
	}

	// Not voice stealing - execute immediately.
	_execute_note_on_immediate();
}

void SiOPMChannelSampler::_execute_note_on_immediate() {
	_stop_click_guard();
	_reset_amp_envelope();

	if (_sampler_table.is_valid()) {
		_sample_data = _sampler_table->get_sample(_wave_number & 127);
	}
	if (_sample_data.is_valid() && _sample_start_phase != 255) {
		_sample_index = _sample_data->get_initial_sample_index(_sample_start_phase * 0.00390625); // 1/256
		_sample_index_fp = (double)_sample_index;
		_sample_pan = _sample_data->get_pan();
		
		// Use unified pitch calculation
		_note_on_pitch = get_pitch();
		_has_note_on_pitch = true;
		_recalc_pitch_step();
	}

	_is_idling = (_sample_data == nullptr);
	_is_note_on = !_is_idling;

	// Start LFO/filter EG like other channels once we know we're active.
	if (!_is_idling) {
		SiOPMChannelBase::note_on();
		_start_amp_envelope();
	}
}

void SiOPMChannelSampler::note_off() {
	if (_sample_data.is_null() || _sample_data->get_ignore_note_off()) {
		return;
	}

	_is_note_on = false;
	_begin_amp_release();

	// Trigger filter EG release like other channels.
	SiOPMChannelBase::note_off();
}

void SiOPMChannelSampler::buffer(int p_length) {
	if (_is_idling || _sample_data == nullptr || _sample_data->get_length() <= 0) {
		buffer_no_process(p_length);
		return;
	}

	Vector<double> wave_data = _sample_data->get_wave_data();
	int channels = _sample_data->get_channel_count();
	int end_point = _sample_data->get_end_point();
	int loop_point = _sample_data->get_loop_point();
	double sample_gain = _sample_data->get_gain_linear();

	// Preserve the start of output pipes.
	SinglyLinkedList<int>::Element *left_start = _out_pipe->get();
	SinglyLinkedList<int>::Element *left_write = left_start;
	SinglyLinkedList<int>::Element *right_start = nullptr;
	SinglyLinkedList<int>::Element *right_write = nullptr;
	if (channels == 2) {
		if (!_out_pipe2) {
			_out_pipe2 = _sound_chip->get_pipe(3, _buffer_index);
		}
		right_start = _out_pipe2->get();
		right_write = right_start;
	}

	// Generate into integer pipes.
	for (int i = 0; i < p_length; i++) {
		// End/loop handling.
		if (_sample_index_fp >= end_point) {
			if (loop_point >= 0) {
				_sample_index_fp = loop_point + (_sample_index_fp - end_point);
			} else {
				_begin_click_guard();
				_is_idling = true;
				// Fill remainder with silence.
				for (; i < p_length; i++) {
					left_write->value = 0;
					left_write = left_write->next();
					if (channels == 2 && right_write) {
						right_write->value = 0;
						right_write = right_write->next();
					}
				}
				break;
			}
		}

		// LFO and ADSR updates.
		_update_lfo();
		_update_amp_envelope();
		if (_amp_stage == AMP_STAGE_IDLE) {
			if (!_click_guard_active) {
				for (; i < p_length; i++) {
					left_write->value = 0;
					left_write = left_write->next();
					if (channels == 2 && right_write) {
						right_write->value = 0;
						right_write = right_write->next();
					}
				}
				break;
			} else {
				left_write->value = 0;
				left_write = left_write->next();
				if (channels == 2 && right_write) {
					right_write->value = 0;
					right_write = right_write->next();
				}
				continue;
			}
		}

		// Interpolation indices.
		int base_index = (int)_sample_index_fp;
		double frac = _sample_index_fp - base_index;

		// Gather samples and interpolate.
		double sL1 = wave_data[(base_index * channels) + 0];
		double sL2 = (base_index + 1 < end_point) ? wave_data[((base_index + 1) * channels) + 0] : sL1;
		double sampleL = sL1 + (sL2 - sL1) * frac;
		double sampleR = sampleL;
		if (channels == 2) {
			double sR1 = wave_data[(base_index * channels) + 1];
			double sR2 = (base_index + 1 < end_point) ? wave_data[((base_index + 1) * channels) + 1] : sR1;
			sampleR = sR1 + (sR2 - sR1) * frac;
		}

		// Apply channel ADSR + AM depth.
		double env = _envelope_level;
		double outL = sampleL * env * _amplitude_modulation_gain * sample_gain;
		double outR = sampleR * env * _amplitude_modulation_gain * sample_gain;

		// Convert to engine int domain and write.
		int vL = CLAMP((int)(outL * 8192.0), -8192, 8191);
		left_write->value = vL;
		left_write = left_write->next();
		if (channels == 2 && right_write) {
			int vR = CLAMP((int)(outR * 8192.0), -8192, 8191);
			right_write->value = vR;
			right_write = right_write->next();
		}

		// Advance sample position with pitch modulation (vibrato).
		double pm_semitones = _pitch_modulation_output_level * (1.0 / 64.0);
		double pitch_factor = std::pow(2.0, pm_semitones / 12.0);
		double step = _pitch_step * pitch_factor;
		_sample_index_fp += step;
	}

	// Apply filter on output pipes.
	if (_filter_on) {
		_apply_sv_filter(left_start, p_length, _filter_variables);
		if (channels == 2 && right_start) {
			_apply_sv_filter(right_start, p_length, _filter_variables2);
		}
	}

	// Write to streams.
	if (!_mute) {
		if (channels == 1) {
			_write_stream_mono(left_start, p_length);
		} else if (right_start) {
			_write_stream_stereo(left_start, right_start, p_length);
		}
	}

	// Metering: copy post-filter mono lane (or left) into meter ring.
	// Advance pipe cursors for next buffer.
	_out_pipe->set(left_write);
	if (channels == 2 && right_write) {
		_out_pipe2->set(right_write);
	}

	_buffer_index += p_length;
}

void SiOPMChannelSampler::buffer_no_process(int p_length) {
	// If we have a deferred note_on waiting, continue updating the amp envelope
	// even without sample data. Otherwise the amp level never decreases and the
	// deferred note_on never executes.
	if (_has_deferred_note_on) {
		for (int i = 0; i < p_length; i++) {
			_update_amp_envelope();
			if (!_has_deferred_note_on) {
				break;
			}
		}
		
		// If the deferred note_on was executed, the channel state has changed.
		if (!_has_deferred_note_on) {
			return;
		}
	}
	
	// Rotate the output buffers similarly to PCM to keep both pipes aligned.
	int pipe_index = (_buffer_index + p_length) & (_sound_chip->get_buffer_length() - 1);
	if (_output_mode == OutputMode::OUTPUT_STANDARD) {
		_out_pipe = _sound_chip->get_pipe(4, pipe_index);
		_out_pipe2 = _sound_chip->get_pipe(3, pipe_index);
		_base_pipe = _sound_chip->get_zero_buffer();
	} else {
		if (_out_pipe) {
			_out_pipe->advance(p_length);
		}
		if (_out_pipe2) {
			_out_pipe2->advance(p_length);
		}
		_base_pipe = (_output_mode == OutputMode::OUTPUT_ADD ? _out_pipe : _sound_chip->get_zero_buffer());
	}

	// Rotate the input buffer when connected by @i.
	if (_input_mode == InputMode::INPUT_PIPE && _in_pipe) {
		_in_pipe->advance(p_length);
	}

	// Rotate the ring buffer.
	if (_ring_pipe) {
		_ring_pipe->advance(p_length);
	}

	// Maintain meter ring position even if idling or no processing.
	_buffer_index += p_length;
}

//

void SiOPMChannelSampler::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);
	reset();
	_out_pipe2 = _sound_chip->get_pipe(3, p_buffer_index);
	_filter_variables2[0] = 0;
	_filter_variables2[1] = 0;
	_filter_variables2[2] = 0;
}

void SiOPMChannelSampler::reset() {
	_is_note_on = false;
	_is_idling = true;

	_bank_number = 0;
	_wave_number = -1;
	_expression = 1;

	_sampler_table = _table->sampler_tables[0];
	// NOTE: Don't clear _sample_data here - preserve it across resets for voice stealing declick.

	_sample_start_phase = 0;
	_sample_index = 0;
	_sample_pan = 0;

	_fine_pitch = 0;
	_note_on_pitch = 0;
	_has_note_on_pitch = false;
	_pitch_step = 1.0;
	_sample_index_fp = 0.0;
	_stop_click_guard();
	_reset_amp_envelope();

	// Clear voice-stealing deferred state.
	_has_deferred_note_on = false;
	_deferred_wave_number = -1;
	_deferred_sample_start_phase = 0;
	_deferred_pitch_step = 1.0;
}

void SiOPMChannelSampler::set_amp_attack_rate(int p_value) {
	int clamped = CLAMP(p_value, 0, 63);
	if (_amp_attack_rate == clamped) {
		return;
	}
	_amp_attack_rate = clamped;
	if (_amp_stage == AMP_STAGE_ATTACK) {
		_configure_amp_stage(1.0, _amp_attack_rate);
	}
}

void SiOPMChannelSampler::set_amp_decay_rate(int p_value) {
	int clamped = CLAMP(p_value, 0, 63);
	if (_amp_decay_rate == clamped) {
		return;
	}
	_amp_decay_rate = clamped;
	if (_amp_stage == AMP_STAGE_DECAY) {
		double sustain = (double)_amp_sustain_level * 0.0078125; // 1/128
		_configure_amp_stage(sustain, _amp_decay_rate);
	}
}

void SiOPMChannelSampler::set_amp_sustain_level(int p_value) {
	int clamped = CLAMP(p_value, 0, 128);
	if (_amp_sustain_level == clamped) {
		return;
	}
	_amp_sustain_level = clamped;
	double sustain = (double)_amp_sustain_level * 0.0078125;
	if (_amp_stage == AMP_STAGE_DECAY) {
		_configure_amp_stage(sustain, _amp_decay_rate);
	} else if (_amp_stage == AMP_STAGE_SUSTAIN) {
		_amp_level = sustain;
		_envelope_level = _amp_level;
	}
}

void SiOPMChannelSampler::set_amp_release_rate(int p_value) {
	int clamped = CLAMP(p_value, 0, 63);
	if (_amp_release_rate == clamped) {
		return;
	}
	_amp_release_rate = clamped;
	if (_amp_stage == AMP_STAGE_RELEASE) {
		_configure_amp_stage(0.0, _amp_release_rate);
	}
}

void SiOPMChannelSampler::_reset_amp_envelope() {
	_amp_stage = AMP_STAGE_IDLE;
	_amp_level = 0.0;
	_amp_stage_target_level = 0.0;
	_amp_stage_increment = 0.0;
	_amp_stage_samples_left = 0;
	_envelope_level = 0.0;
	_is_idling = true;
}

void SiOPMChannelSampler::_start_amp_envelope() {
	_stop_click_guard();
	_is_idling = false;
	_set_amp_stage(AMP_STAGE_ATTACK);
}

void SiOPMChannelSampler::_begin_amp_release() {
	if (_amp_stage == AMP_STAGE_IDLE || _amp_stage == AMP_STAGE_RELEASE) {
		return;
	}
	_set_amp_stage(AMP_STAGE_RELEASE);
}

void SiOPMChannelSampler::_advance_amp_stage() {
	AmplitudeStage old_stage = _amp_stage;
	
	switch (_amp_stage) {
		case AMP_STAGE_ATTACK: {
			// Skip decay if sustain is full scale and decay rate is zero.
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
			_begin_click_guard();
		} break;
		default:
			break;
	}
}

void SiOPMChannelSampler::_set_amp_stage(AmplitudeStage p_stage) {
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
			_is_idling = true;
		} break;
	}
}

void SiOPMChannelSampler::_configure_amp_stage(double p_target_level, int p_rate) {
	_amp_stage_target_level = CLAMP(p_target_level, 0.0, 1.0);
	double delta = _amp_stage_target_level - _amp_level;
	double delta_abs = Math::abs(delta);
	bool immediate = (p_rate < 0) || Math::is_zero_approx(delta_abs);
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

	double units = Math::ceil(delta_abs * 128.0);
	if (units < 1.0) {
		units = 1.0;
	}
	_amp_stage_samples_left = (int)Math::ceil(samples_per_unit * units);
	if (_amp_stage_samples_left <= 0) {
		_amp_stage_samples_left = 1;
	}
	_amp_stage_increment = delta / (double)_amp_stage_samples_left;
}

void SiOPMChannelSampler::_refresh_active_amp_stage() {
	switch (_amp_stage) {
		case AMP_STAGE_ATTACK:
			_configure_amp_stage(1.0, _amp_attack_rate);
			break;
		case AMP_STAGE_DECAY: {
			double sustain = (double)_amp_sustain_level * 0.0078125;
			_configure_amp_stage(sustain, _amp_decay_rate);
		} break;
		case AMP_STAGE_RELEASE:
			_configure_amp_stage(0.0, _amp_release_rate);
			break;
		default:
			break;
	}
}

int SiOPMChannelSampler::_compute_amp_samples_per_unit(int p_rate) const {
	int rate_index = CLAMP(p_rate, 0, 63);
	int base = _table->filter_eg_rate[rate_index];
	if (base <= 0) {
		int slowest_ref = _table->filter_eg_rate[1];
		if (slowest_ref <= 0) {
			slowest_ref = 32768;
		}
		base = slowest_ref << 4;
	}
	double scaled = base * _amp_rate_scale;
	if (scaled <= 0.0) {
		return 0;
	}
	int samples = (int)scaled;
	return samples > 0 ? samples : 1;
}

void SiOPMChannelSampler::_update_amp_envelope() {
	switch (_amp_stage) {
		case AMP_STAGE_ATTACK:
		case AMP_STAGE_DECAY:
		case AMP_STAGE_RELEASE: {
			if (_amp_stage_samples_left > 0) {
				_amp_level += _amp_stage_increment;
				_amp_stage_samples_left--;
				
				// Check if we have a deferred note_on waiting and we're quiet enough.
				// Must check BEFORE _advance_amp_stage() which transitions to IDLE.
				if (_has_deferred_note_on && _amp_stage == AMP_STAGE_RELEASE && _amp_level < 0.1) {
					// Restore deferred parameters.
					_wave_number = _deferred_wave_number;
					_sample_start_phase = _deferred_sample_start_phase;
					_pitch_step = _deferred_pitch_step;
					_has_deferred_note_on = false;

					// Now execute the actual note_on.
					_execute_note_on_immediate();
					return; // Early return since envelope state was reset
				}
				
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
			// Safety net: if we somehow ended up in IDLE with a deferred note_on still pending,
			// execute it now to prevent permanent silence.
			if (_has_deferred_note_on) {
				_wave_number = _deferred_wave_number;
				_sample_start_phase = _deferred_sample_start_phase;
				_pitch_step = _deferred_pitch_step;
				_has_deferred_note_on = false;
				_execute_note_on_immediate();
				return;
			}
			break;
	}

	_envelope_level = CLAMP(_amp_level, 0.0, 1.0);
	if (_click_guard_active) {
		if (_click_guard_samples_left > 0) {
			_click_guard_samples_left--;
			_click_guard_level = (double)_click_guard_samples_left / RELEASE_SAMPLES;
			_envelope_level *= _click_guard_level;
		} else {
			_stop_click_guard();
			_envelope_level = 0.0;
		}
	}
}

void SiOPMChannelSampler::_begin_click_guard() {
	_click_guard_active = true;
	_click_guard_samples_left = RELEASE_SAMPLES;
	_click_guard_level = 1.0;
}

void SiOPMChannelSampler::_stop_click_guard() {
	_click_guard_active = false;
	_click_guard_samples_left = 0;
	_click_guard_level = 1.0;
}

void SiOPMChannelSampler::_write_stream_mono(SinglyLinkedList<int>::Element *p_output, int p_length) {
	double volume_coef = _expression * _sound_chip->get_sampler_volume() * _instrument_gain;
	int pan = CLAMP(_pan + _sample_pan, 0, 128);

	if (_kill_fade_remaining_samples > 0) {
		_apply_kill_fade(p_output, p_length);
	}

	if (_has_effect_send) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
				if (stream) {
					stream->write(p_output, _buffer_index, p_length, _volumes[i] * volume_coef, pan);
				}
			}
		}
	} else {
		SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
		stream->write(p_output, _buffer_index, p_length, _volumes[0] * volume_coef, pan);
	}
}

void SiOPMChannelSampler::_write_stream_stereo(SinglyLinkedList<int>::Element *p_output_left, SinglyLinkedList<int>::Element *p_output_right, int p_length) {
	double volume_coef = _expression * _sound_chip->get_sampler_volume() * _instrument_gain;
	int pan = CLAMP(_pan + _sample_pan, 0, 128);

	if (_kill_fade_remaining_samples > 0) {
		_apply_kill_fade_stereo(p_output_left, p_output_right, p_length);
	}

	if (_has_effect_send) {
		for (int i = 0; i < SiOPMSoundChip::STREAM_SEND_SIZE; i++) {
			if (_volumes[i] > 0) {
				SiOPMStream *stream = _streams[i] ? _streams[i] : _sound_chip->get_stream_slot(i);
				if (stream) {
					stream->write_stereo(p_output_left, p_output_right, _buffer_index, p_length, _volumes[i] * volume_coef, pan);
				}
			}
		}
	} else {
		SiOPMStream *stream = _streams[0] ? _streams[0] : _sound_chip->get_output_stream();
		stream->write_stereo(p_output_left, p_output_right, _buffer_index, p_length, _volumes[0] * volume_coef, pan);
	}
}

String SiOPMChannelSampler::_to_string() const {
	String params = "";

	params += "vol=" + rtos(_volumes[0] * _expression) + ", ";
	params += "pan=" + itos(_pan - 64) + "";

	return "SiOPMChannelSampler: " + params;
}

// Sampler-specific live param setters.

void SiOPMChannelSampler::_recalc_pitch_step() {
	if (!_sample_data.is_valid()) {
		return;
	}
	
	// Layer 1: MIDI note transposition (ignored if fixed_pitch)
	double note_transposition = 0.0;
	if (!_sample_data->is_fixed_pitch()) {
		note_transposition = (double)(_wave_number - 60);
	}
	
	// Layer 2: User pitch adjustments from SamplerData (set via param system)
	double user_offset = (double)_sample_data->get_root_offset() 
	                   + (double)_sample_data->get_coarse_offset() 
	                   + ((double)_sample_data->get_fine_offset() / 100.0);
	
	// Combine all layers
	double total_pitch = note_transposition + user_offset;
	_pitch_step = std::pow(2.0, total_pitch / 12.0);
}

void SiOPMChannelSampler::set_sampler_start_point(int p_start) {
	if (_sample_data.is_valid()) {
		_sample_data->set_start_point(p_start);
	}
}

void SiOPMChannelSampler::set_sampler_end_point(int p_end) {
	if (_sample_data.is_valid()) {
		_sample_data->set_end_point(p_end);
	}
}

void SiOPMChannelSampler::set_sampler_loop_point(int p_loop) {
	if (_sample_data.is_valid()) {
		_sample_data->set_loop_point(p_loop);
	}
}

void SiOPMChannelSampler::set_sampler_ignore_note_off(bool p_ignore) {
	if (_sample_data.is_valid()) {
		_sample_data->set_ignore_note_off(p_ignore);
	}
}

void SiOPMChannelSampler::set_sampler_pan(int p_pan) {
	if (_sample_data.is_valid()) {
		_sample_data->set_pan(p_pan);
		// Update the channel's sample pan immediately.
		_sample_pan = p_pan;
	}
}

void SiOPMChannelSampler::set_sampler_gain_db(int p_db) {
	if (_sample_data.is_valid()) {
		_sample_data->set_gain_db(p_db);
	}
}

int SiOPMChannelSampler::get_sampler_gain_db() const {
	return _sample_data.is_valid() ? _sample_data->get_gain_db() : 0;
}

void SiOPMChannelSampler::set_sampler_root_offset(int p_semitones) {
	if (_sample_data.is_valid()) {
		_sample_data->set_root_offset(p_semitones);
		_recalc_pitch_step();
	}
}

void SiOPMChannelSampler::set_sampler_coarse_offset(int p_semitones) {
	if (_sample_data.is_valid()) {
		_sample_data->set_coarse_offset(p_semitones);
		_recalc_pitch_step();
	}
}

void SiOPMChannelSampler::set_sampler_fine_offset(int p_cents) {
	if (_sample_data.is_valid()) {
		_sample_data->set_fine_offset(p_cents);
		_recalc_pitch_step();
	}
}

int SiOPMChannelSampler::get_sampler_root_offset() const {
	return _sample_data.is_valid() ? _sample_data->get_root_offset() : 0;
}

int SiOPMChannelSampler::get_sampler_coarse_offset() const {
	return _sample_data.is_valid() ? _sample_data->get_coarse_offset() : 0;
}

int SiOPMChannelSampler::get_sampler_fine_offset() const {
	return _sample_data.is_valid() ? _sample_data->get_fine_offset() : 0;
}

SiOPMChannelSampler::SiOPMChannelSampler(SiOPMSoundChip *p_chip) : SiOPMChannelBase(p_chip) {
	// Empty.
}
