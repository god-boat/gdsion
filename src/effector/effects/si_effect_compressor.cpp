/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_compressor.h"

#include "chip/siopm_ref_table.h"

void SiEffectCompressor::set_params(double p_threshold, double p_window_time, double p_attack_time, double p_release_time, double p_max_gain, double p_mixing_level) {
	std::lock_guard<std::mutex> guard(_state_mutex);

	_threshold_squared = p_threshold * p_threshold;

	double samples_per_ms = 48.0;
	SiOPMRefTable *ref_table = SiOPMRefTable::get_instance();
	if (ref_table && ref_table->sampling_rate > 0) {
		samples_per_ms = ref_table->sampling_rate / 1000.0;
	}

	_window_samples = (int)(p_window_time * samples_per_ms);
	if (_window_samples <= 0) {
		_window_samples = 1;
	}
	_window_rms_averaging = 1.0 / _window_samples;

	if (_window_rms_list) {
		memdelete(_window_rms_list);
	}
	_window_rms_list = memnew(SinglyLinkedList<double>(_window_samples, 0.0, true));

	_attack_rate = 0.5;
	if (p_attack_time != 0) {
		double attack_samples = p_attack_time * samples_per_ms;
		if (attack_samples > 0) {
			_attack_rate = Math::pow(2, -1.0 / attack_samples);
		}
	}

	_release_rate = 2.0;
	if (p_release_time != 0) {
		double release_samples = p_release_time * samples_per_ms;
		if (release_samples > 0) {
			_release_rate = Math::pow(2, 1.0 / release_samples);
		}
	}

	_max_gain = Math::pow(2, -p_max_gain / 6.0);
	_mixing_level = p_mixing_level;
}

int SiEffectCompressor::prepare_process() {
	std::lock_guard<std::mutex> guard(_state_mutex);

	if (!_window_rms_list) {
		return 2;
	}

	_window_rms_list->reset();
	_window_rms_total = 0;
	_gain = 2;

	return 2;
}

int SiEffectCompressor::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	std::lock_guard<std::mutex> guard(_state_mutex);

	if (!_window_rms_list) {
		return p_channels;
	}

	int start_index = p_start_index << 1;
	int length = p_length << 1;

	for (int i = start_index; i < (start_index + length); i += 2) {
		double value_left = (*r_buffer)[i];
		double value_right = (*r_buffer)[i + 1];

		_window_rms_list->next();
		_window_rms_total -= _window_rms_list->get()->value;
		_window_rms_list->get()->value = value_left * value_left + value_right * value_right;
		_window_rms_total += _window_rms_list->get()->value;

		double rms_value = _window_rms_total * _window_rms_averaging;
		_gain *= (rms_value > _threshold_squared ? _attack_rate : _release_rate);
		if (_gain > _max_gain) {
			_gain = _max_gain;
		}

		value_left = CLAMP(value_left * _gain, -1, 1);
		value_right = CLAMP(value_right * _gain, -1, 1);

		r_buffer->write[i] = value_left * _mixing_level;
		r_buffer->write[i + 1] = value_right * _mixing_level;
	}

	return p_channels;
}

void SiEffectCompressor::set_by_mml(Vector<double> p_args) {
	double threshold    = _get_mml_arg(p_args, 0, 70) / 100.0;
	double window_time  = _get_mml_arg(p_args, 1, 50);
	double attack_time  = _get_mml_arg(p_args, 2, 20);
	double release_time = _get_mml_arg(p_args, 3, 20);
	double max_gain     = _get_mml_arg(p_args, 4, -6);
	double mixing_level = _get_mml_arg(p_args, 5, 50) / 100.0;

	set_params(threshold, window_time, attack_time, release_time, max_gain, mixing_level);
}

void SiEffectCompressor::reset() {
	set_params();
}

void SiEffectCompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "threshold", "window_time", "attack_time", "release_time", "max_gain", "mixing_level"), &SiEffectCompressor::set_params, DEFVAL(0.7), DEFVAL(50), DEFVAL(20), DEFVAL(20), DEFVAL(-6), DEFVAL(0.5));
}

SiEffectCompressor::SiEffectCompressor(double p_threshold, double p_window_time, double p_attack_time, double p_release_time, double p_max_gain, double p_mixing_level) :
		SiEffectBase() {
	set_params(p_threshold, p_window_time, p_attack_time, p_release_time, p_max_gain, p_mixing_level);
}
