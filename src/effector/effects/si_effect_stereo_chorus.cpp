/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_stereo_chorus.h"

using sion::dsp::FractionalDelay;
using sion::dsp::LfoShape;

void SiEffectStereoChorus::set_params(double p_delay_time, double p_feedback, double p_frequency, double p_depth, double p_wet, bool p_invert_phase) {
	ERR_FAIL_COND_MSG(p_delay_time == 0, "SiEffectStereoChorus: Delay cannot be zero.");

	_center_delay = MIN(p_delay_time * _get_samples_per_ms(), (double)(MAX_DELAY_SAMPLES / 2));
	_depth = MIN(p_depth, _center_delay - 4);
	_feedback = CLAMP(p_feedback, -0.9990234375, 0.9990234375);
	_lfo.set_hz(p_frequency, _get_sampling_rate());
	_phase_invert = p_invert_phase ? -1.0 : 1.0;

	_calculate_constant_power_gains(p_wet, _dry_gain, _wet_gain);
}

int SiEffectStereoChorus::prepare_process() {
	_delay_left.clear();
	_delay_right.clear();
	_lfo.reset();

	return 2;
}

double SiEffectStereoChorus::_process_channel(FractionalDelay &r_delay, double p_input, double p_delay_samples) {
	double delayed = r_delay.read(p_delay_samples);
	r_delay.write(p_input - delayed * _feedback);
	return p_input * _dry_gain + delayed * _wet_gain;
}

int SiEffectStereoChorus::process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	double *buffer = r_buffer->ptrw();
	const int end = (p_start_index + p_length) << 1;
	for (int i = p_start_index << 1; i < end; i += 2) {
		double swing = _depth * _lfo.tick<LfoShape::SINE>();
		buffer[i] = _process_channel(_delay_left, buffer[i], _center_delay - swing);
		buffer[i + 1] = _process_channel(_delay_right, buffer[i + 1], _center_delay - swing * _phase_invert);
	}

	return p_channels;
}

void SiEffectStereoChorus::set_by_mml(Vector<double> p_args) {
	double delay_time = _get_mml_arg(p_args, 0, 20);
	double feedback   = _get_mml_arg(p_args, 1, 20) / 100.0;
	double frequency  = _get_mml_arg(p_args, 2, 4);
	double depth      = _get_mml_arg(p_args, 3, 20);
	double wet        = _get_mml_arg(p_args, 4, 50) / 100.0;
	int invert_phase  = _get_mml_arg(p_args, 5, 0);

	set_params(delay_time, feedback, frequency, depth, wet, invert_phase != 0);
}

void SiEffectStereoChorus::reset() {
	set_params();
}

void SiEffectStereoChorus::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "delay_time", "feedback", "frequency", "depth", "wet", "invert_phase"), &SiEffectStereoChorus::set_params, DEFVAL(20), DEFVAL(0.2), DEFVAL(4), DEFVAL(20), DEFVAL(0.5), DEFVAL(true));
}

SiEffectStereoChorus::SiEffectStereoChorus(double p_delay_time, double p_feedback, double p_frequency, double p_depth, double p_wet, bool p_invert_phase) :
		SiEffectBase() {
	_delay_left.prepare(MAX_DELAY_SAMPLES);
	_delay_right.prepare(MAX_DELAY_SAMPLES);

	set_params(p_delay_time, p_feedback, p_frequency, p_depth, p_wet, p_invert_phase);
}
