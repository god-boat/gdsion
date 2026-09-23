/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_autopan.h"

void SiEffectAutopan::set_params(double p_frequency, double p_stereo_width) {
	const double step = 2.0 * M_PI * p_frequency / _get_sampling_rate();
	const double offset = M_PI * p_stereo_width;
	_step_cos = Math::cos(step);
	_step_sin = Math::sin(step);
	_offset_cos = Math::cos(offset);
	_offset_sin = Math::sin(offset);
}

int SiEffectAutopan::prepare_process() {
	return 2;
}

int SiEffectAutopan::process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	const int end_index = (p_start_index + p_length) << 1;
	double lfo_cos = _lfo_cos;
	double lfo_sin = _lfo_sin;
	for (int i = p_start_index << 1; i < end_index; i += 2) {
		const double left_gain = 0.5 * (1.0 + lfo_cos);
		const double right_gain = 0.5 * (1.0 + lfo_cos * _offset_cos - lfo_sin * _offset_sin);
		r_buffer->write[i] = (*r_buffer)[i] * left_gain;
		r_buffer->write[i + 1] = (*r_buffer)[i + 1] * right_gain;

		const double next_cos = lfo_cos * _step_cos - lfo_sin * _step_sin;
		lfo_sin = lfo_sin * _step_cos + lfo_cos * _step_sin;
		lfo_cos = next_cos;
	}
	const double amplitude = Math::sqrt(lfo_cos * lfo_cos + lfo_sin * lfo_sin);
	_lfo_cos = lfo_cos / amplitude;
	_lfo_sin = lfo_sin / amplitude;

	return 2;
}

void SiEffectAutopan::set_by_mml(Vector<double> p_args) {
	double frequency = _get_mml_arg(p_args, 0, 1);
	double stereo_width = _get_mml_arg(p_args, 1, 100) / 100.0;

	set_params(frequency, stereo_width);
}

void SiEffectAutopan::reset() {
	_lfo_cos = 1;
	_lfo_sin = 0;
	set_params();
}

void SiEffectAutopan::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "frequency", "stereo_width"), &SiEffectAutopan::set_params, DEFVAL(1), DEFVAL(1));
}

SiEffectAutopan::SiEffectAutopan(double p_frequency, double p_stereo_width) :
		SiEffectBase() {
	set_params(p_frequency, p_stereo_width);
}
