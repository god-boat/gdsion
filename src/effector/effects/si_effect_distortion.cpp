/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_distortion.h"

const double SiEffectDistortion::THRESHOLD = 0.0000152587890625;

void SiEffectDistortion::set_params(double p_pre_gain, double p_post_gain, double p_lpf_frequency, double p_lpf_slope) {
	_limit = Math::pow(2, -p_post_gain / 6.0);
	_pre_scale = Math::pow(2, -p_pre_gain / 6.0) * _limit;
	_filter_enabled = p_lpf_frequency > 0;

	if (_filter_enabled) {
		// TODO: Pick better names for these variables.

		double omg = _get_angular_frequency(p_lpf_frequency);
		double cos = Math::cos(omg);
		double sin = Math::sin(omg);

		double ang = 0.34657359027997264 * p_lpf_slope * omg / sin;
		double alp = sin * Math::sinh(ang); // log(2)*0.5
		double ia0 = 1.0 / (1.0 + alp);

		_lpf_coeffs.a1 = -2 * cos * ia0;
		_lpf_coeffs.a2 = (1 - alp) * ia0;
		_lpf_coeffs.b1 = (1 - cos) * ia0;
		_lpf_coeffs.b0 = _lpf_coeffs.b1 * 0.5;
		_lpf_coeffs.b2 = _lpf_coeffs.b1 * 0.5;
	}
}

void SiEffectDistortion::process(const ProcessContext &p_context, Vector<double> *r_buffer, int p_start_index, int p_length) {
	int start_index = p_start_index << 1;
	int length = p_length << 1;

	for (int i = start_index; i < (start_index + length); i++) {
		double value = CLAMP((*r_buffer)[i] * _pre_scale, -_limit, _limit);

		double output = value;
		if (_filter_enabled) {
			sion::dsp::BiquadState &state = _lpf_state[i & 1];
			if (Math::abs(state.y1) < THRESHOLD) {
				state.y1 = 0;
				state.y2 = 0;
			}

			output = state.tick(_lpf_coeffs, value);
		}

		r_buffer->write[i] = output;
	}
}

void SiEffectDistortion::set_by_mml(Vector<double> p_args) {
	double pre_gain       = _get_mml_arg(p_args, 0, -60);
	double post_gain      = _get_mml_arg(p_args, 1, 18);
	double lpf_frequency  = _get_mml_arg(p_args, 2, 2400);
	double lpf_slope      = _get_mml_arg(p_args, 3, 1);

	set_params(pre_gain, post_gain, lpf_frequency, lpf_slope);
}

void SiEffectDistortion::reset() {
	for (sion::dsp::BiquadState &state : _lpf_state) {
		state = {};
	}
	set_params();
}

void SiEffectDistortion::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "pre_gain", "post_gain", "lpf_frequency", "lpf_slope"), &SiEffectDistortion::set_params, DEFVAL(-60),  DEFVAL(18),  DEFVAL(2400),  DEFVAL(1));
}

SiEffectDistortion::SiEffectDistortion(double p_pre_gain, double p_post_gain, double p_lpf_frequency, double p_lpf_slope) :
		SiEffectBase() {
	set_params(p_pre_gain, p_post_gain, p_lpf_frequency, p_lpf_slope);
}
