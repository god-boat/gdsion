/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_stereo_delay.h"

#include "chip/siopm_ref_table.h"

void SiEffectStereoDelay::set_params(double p_delay_time, double p_feedback, bool p_cross, double p_wet, int p_time_mode) {
	double samples_per_ms = 48.0;
	double sampling_rate = 48000.0;
	SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
	if (ref_table && ref_table->sampling_rate > 0) {
		sampling_rate = ref_table->sampling_rate;
		samples_per_ms = sampling_rate / 1000.0;
	}

	int offset = (int)(p_delay_time * samples_per_ms);
	if (offset > DELAY_BUFFER_FILTER) {
		offset = DELAY_BUFFER_FILTER;
	}
	int current_offset = (_pointer_write - _pointer_read) & DELAY_BUFFER_FILTER;
	
	_time_mode = static_cast<DelayTimeMode>(p_time_mode);
	
	if (current_offset != offset) {
		_pointer_read_old = _pointer_read;
		_pointer_read_target = (_pointer_write - offset) & DELAY_BUFFER_FILTER;
		_pointer_read_fractional = 0.0;
		_crossfade_position = 0.0;
		int crossfade_samples = (int)(CROSSFADE_TIME * sampling_rate);
		if (crossfade_samples > 0) {
			_crossfade_increment = 1.0 / crossfade_samples;
		} else {
			_crossfade_increment = 1.0;
		}
	}

	_feedback = p_feedback;
	if (_feedback >= 1) {
		_feedback = 0.9990234375;
	} else if (_feedback <= -1) {
		_feedback = -0.9990234375;
	}

	_wet = p_wet;
	_cross = p_cross;

	_calculate_constant_power_gains(_wet, _dry_gain, _wet_gain);
}

int SiEffectStereoDelay::prepare_process() {
	_delay_buffer_left.fill(0);
	_delay_buffer_right.fill(0);

	return 2;
}

void SiEffectStereoDelay::_process_channel(Vector<double> *r_buffer, int p_buffer_index, Vector<double> *p_read_buffer, Vector<double> *r_write_buffer) {
	double value;
	
	if (_crossfade_position < 1.0 && _time_mode == DELAY_TIME_MODE_FADE) {
		double old_value = (*p_read_buffer)[_pointer_read_old];
		double new_value = (*p_read_buffer)[_pointer_read_target];
		value = old_value * (1.0 - _crossfade_position) + new_value * _crossfade_position;
	} else {
		value = (*p_read_buffer)[_pointer_read];
	}
	
	r_write_buffer->write[_pointer_write] = (*r_buffer)[p_buffer_index] - value * _feedback;
	r_buffer->write[p_buffer_index] = (*r_buffer)[p_buffer_index] * _dry_gain + value * _wet_gain;
}

int SiEffectStereoDelay::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	int start_index = p_start_index << 1;
	int length = p_length << 1;

	for (int i = start_index; i < (start_index + length); i += 2) {
		_process_channel(r_buffer, i,     (_cross ? &_delay_buffer_right : &_delay_buffer_left), &_delay_buffer_left);
		_process_channel(r_buffer, i + 1, (_cross ? &_delay_buffer_left : &_delay_buffer_right), &_delay_buffer_right);

		_pointer_write = (_pointer_write + 1) & DELAY_BUFFER_FILTER;
		
		if (_crossfade_position < 1.0) {
			_crossfade_position += _crossfade_increment;
			if (_crossfade_position >= 1.0) {
				_crossfade_position = 1.0;
				_pointer_read = _pointer_read_target;
				_pointer_read_fractional = 0.0;
			}
			
			if (_time_mode == DELAY_TIME_MODE_PITCH) {
				int distance = (_pointer_read_target - _pointer_read_old) & DELAY_BUFFER_FILTER;
				if (distance > (DELAY_BUFFER_FILTER >> 1)) {
					distance = distance - DELAY_BUFFER_FILTER - 1;
				}
				double step = 1.0 + distance * _crossfade_increment;
				_pointer_read_fractional += step;
				
				int step_int = (int)_pointer_read_fractional;
				_pointer_read_fractional -= step_int;
				_pointer_read = (_pointer_read + step_int) & DELAY_BUFFER_FILTER;
			}
			
			_pointer_read_old = (_pointer_read_old + 1) & DELAY_BUFFER_FILTER;
			_pointer_read_target = (_pointer_read_target + 1) & DELAY_BUFFER_FILTER;
		} else {
			_pointer_read = (_pointer_read + 1) & DELAY_BUFFER_FILTER;
		}
	}

	return p_channels;
}

void SiEffectStereoDelay::set_by_mml(Vector<double> p_args) {
	double delay_time = _get_mml_arg(p_args, 0, 250);
	double feedback   = _get_mml_arg(p_args, 1, 25) / 100.0;
	int cross         = _get_mml_arg(p_args, 2, 0);
	double wet        = _get_mml_arg(p_args, 3, 100) / 100.0;
	int time_mode     = _get_mml_arg(p_args, 4, DELAY_TIME_MODE_FADE);

	set_params(delay_time, feedback, cross == 1, wet, time_mode);
}

void SiEffectStereoDelay::reset() {
	set_params();
}

void SiEffectStereoDelay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "delay_time", "feedback", "is_cross", "wet", "time_mode"), &SiEffectStereoDelay::set_params, DEFVAL(250), DEFVAL(0.25), DEFVAL(false), DEFVAL(0.25), DEFVAL(DELAY_TIME_MODE_FADE));
	
	BIND_ENUM_CONSTANT(DELAY_TIME_MODE_PITCH);
	BIND_ENUM_CONSTANT(DELAY_TIME_MODE_FADE);
}

SiEffectStereoDelay::SiEffectStereoDelay(double p_delay_time, double p_feedback, bool p_cross, double p_wet, int p_time_mode) :
		SiEffectBase() {
	_delay_buffer_left.resize_zeroed(1 << DELAY_BUFFER_BITS);
	_delay_buffer_right.resize_zeroed(1 << DELAY_BUFFER_BITS);

	set_params(p_delay_time, p_feedback, p_cross, p_wet, p_time_mode);
}
