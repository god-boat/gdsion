#include "siopm_channel_strata.h"

#include <cmath>
#include <cstring>
#include <godot_cpp/core/class_db.hpp>
#include "chip/siopm_channel_params.h"
#include "chip/siopm_ref_table.h"
#include "chip/siopm_sound_chip.h"
#include "chip/siopm_stream.h"
#include "templates/singly_linked_list.h"

void SiOPMChannelStrata::_recompute_pitch_correction(double p_sample_rate) {
	if (p_sample_rate <= 0) {
		_pitch_correction = 0;
		return;
	}
	// Braids assumes 96 kHz internally. To make a given MIDI note number sound
	// the same at our host rate, raise the pitch fed to Braids by
	//   log2(REFERENCE_SAMPLE_RATE / host_sr) * 12 semitones * 128 units/semi.
	double semitones = std::log2(REFERENCE_SAMPLE_RATE / p_sample_rate) * 12.0;
	_pitch_correction = (int)std::round(semitones * 128.0);
}

void SiOPMChannelStrata::_set_strata_pitch() {
	// SiON pitch is 64 units / semitone -> Braids is 128 units / semitone.
	int braids_pitch = (_current_pitch << 1) + _pitch_correction;
	// Clamp to a safe range under both analog (kHighestNote=128*128) and
	// digital (kHighestNote=140*128) ceilings.
	if (braids_pitch < 0) {
		braids_pitch = 0;
	} else if (braids_pitch > 16383) {
		braids_pitch = 16383;
	}
	_osc.set_pitch((int16_t)braids_pitch);
}

void SiOPMChannelStrata::set_strata_params(int p_shape, int p_timbre, int p_color) {
	int s = p_shape;
	if (s < 0) {
		s = 0;
	} else if (s >= (int)braids::MACRO_OSC_SHAPE_LAST) {
		s = (int)braids::MACRO_OSC_SHAPE_LAST - 1;
	}
	_shape = s;
	_timbre = CLAMP(p_timbre, 0, 32767);
	_color = CLAMP(p_color, 0, 32767);

	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);
}

void SiOPMChannelStrata::get_channel_params(const Ref<SiOPMChannelParams> &p_params) const {
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

void SiOPMChannelStrata::set_channel_params(const Ref<SiOPMChannelParams> &p_params, bool p_with_volume, bool p_with_modulation) {
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
		p_params->get_filter_release_offset()
	);
}

void SiOPMChannelStrata::offset_volume(int p_expression, int p_velocity) {
	_expression = (double)p_expression * 0.0078125;
}

void SiOPMChannelStrata::note_on() {
	_osc.Strike();
	_is_note_on = true;
	_is_idling = false;
	_declick_target = 1.0;
	SiOPMChannelBase::note_on();
}

void SiOPMChannelStrata::note_off() {
	_is_note_on = false;
	_declick_target = 0.0;
	SiOPMChannelBase::note_off();
}

void SiOPMChannelStrata::reset_channel_buffer_status() {
	SiOPMChannelBase::reset_channel_buffer_status();
	_is_idling = !_is_note_on && _declick_level <= 0.0;
}

void SiOPMChannelStrata::_process_strata(int p_length) {
	SinglyLinkedList<int>::Element *in_pipe   = _in_pipe->get();
	SinglyLinkedList<int>::Element *base_pipe = _base_pipe->get();
	SinglyLinkedList<int>::Element *out_pipe  = _out_pipe->get();

	_set_strata_pitch();
	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);

	// Braids int16 peak = 32768, SiON pipe peak = 1 << LOG_VOLUME_BITS = 8192.
	// Scale factor = 8192/32768 = 0.25, combined with per-note expression.
	const double gain = _expression * 0.25;

	int written = 0;
	while (written < p_length) {
		int chunk = p_length - written;
		if (chunk > BRAIDS_BLOCK_SIZE) {
			chunk = BRAIDS_BLOCK_SIZE;
		}
		// Braids drum shapes decrement size by 2 per iteration; an odd size
		// causes unsigned underflow in size_t. Round up to the next even number
		// for Render, but only read `chunk` samples from the output.
		int render_size = (chunk + 1) & ~1;
		memset(_sync_buffer, 0, sizeof(_sync_buffer));
		_osc.Render(_sync_buffer, _render_buffer, (size_t)render_size);

		for (int i = 0; i < chunk; i++) {
			if (_declick_level < _declick_target) {
				_declick_level = MIN(_declick_level + DECLICK_INCREMENT, _declick_target);
			} else if (_declick_level > _declick_target) {
				_declick_level = MAX(_declick_level - DECLICK_INCREMENT, 0.0);
				if (_declick_level <= 0.0) {
					_is_idling = true;
				}
			}

			int sample = (int)((double)_render_buffer[i] * gain * _declick_level);
			out_pipe->value = sample + base_pipe->value;

			in_pipe   = in_pipe->next();
			base_pipe = base_pipe->next();
			out_pipe  = out_pipe->next();
		}
		written += chunk;
	}

	_in_pipe->set(in_pipe);
	_base_pipe->set(base_pipe);
	_out_pipe->set(out_pipe);
}

void SiOPMChannelStrata::initialize(SiOPMChannelBase *p_prev, int p_buffer_index) {
	SiOPMChannelBase::initialize(p_prev, p_buffer_index);

	double sample_rate = _table ? (double)_table->sampling_rate : REFERENCE_SAMPLE_RATE;
	_recompute_pitch_correction(sample_rate);

	_shape = (int)braids::MACRO_OSC_SHAPE_CSAW;
	_timbre = 0;
	_color = 0;
	_current_pitch = 0;
	_expression = 1.0;
	_declick_level = 0.0;
	_declick_target = 0.0;

	_osc.Init();
	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);

	_process_function = Callable(this, "_process_strata");
}

void SiOPMChannelStrata::reset() {
	_osc.Init();
	_osc.set_shape((braids::MacroOscillatorShape)_shape);
	_osc.set_parameters((int16_t)_timbre, (int16_t)_color);
	_declick_level = 0.0;
	_declick_target = 0.0;

	SiOPMChannelBase::reset();
}

String SiOPMChannelStrata::_to_string() const {
	String params;
	params += "shape=" + itos(_shape) + ", ";
	params += "timbre=" + itos(_timbre) + ", ";
	params += "color=" + itos(_color) + ", ";
	params += "vol=" + rtos(_volumes[0]) + ", ";
	params += "pan=" + itos(_pan - 64);

	return "SiOPMChannelStrata: " + params;
}

void SiOPMChannelStrata::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_process_strata", "length"), &SiOPMChannelStrata::_process_strata);
}

SiOPMChannelStrata::SiOPMChannelStrata(SiOPMSoundChip *p_chip) : SiOPMChannelBase(p_chip) {
	_osc.Init();
	_process_function = Callable(this, "_process_strata");
}
