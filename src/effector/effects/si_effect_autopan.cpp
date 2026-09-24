/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_autopan.h"

using sion::dsp::LfoRateMode;
using sion::dsp::LfoShape;

void SiEffectAutopan::set_params(double p_frequency, double p_stereo_width, int p_rate_mode, int p_sync_division, int p_shape, double p_time) {
	const LfoShape shape = (LfoShape)CLAMP(p_shape, 0, (int)LfoShape::SAMPLE_HOLD);
	const double offset = 0.5 * p_stereo_width;
	const double width_offset = offset - Math::floor(offset);
	_transition_pending = _transition_pending || shape != _shape || width_offset != _width_offset;

	_rate.mode = (LfoRateMode)CLAMP(p_rate_mode, 0, (int)LfoRateMode::TRIPLET);
	_rate.hz = p_frequency;
	_rate.ms = p_time;
	_rate.division = CLAMP(p_sync_division, 0, sion::dsp::BEAT_DIVISION_COUNT - 1);
	_shape = shape;
	_stereo_width = p_stereo_width;

	// A width of 1 puts the two channels half a cycle apart.
	_width_offset = width_offset;
}

int SiEffectAutopan::prepare_process() {
	return 2;
}

template <LfoShape SHAPE>
void SiEffectAutopan::_process(sion::dsp::LfoShapeTag<SHAPE>, double *r_buffer, int p_begin, int p_end) {
	sion::dsp::Lfo lfo = _lfo;
	const double width_offset = _width_offset;
	for (int i = p_begin; i < p_end; i += 2) {
		const double target_left = 0.5 * (1.0 + lfo.read<SHAPE>());
		const double target_right = 0.5 * (1.0 + lfo.read<SHAPE>(width_offset));
		r_buffer[i] *= _left_gain.process(target_left);
		r_buffer[i + 1] *= _right_gain.process(target_right);
		lfo.advance();
	}
	_lfo = lfo;
}

int SiEffectAutopan::process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	const double resolved_hz = _rate.hz_at(p_context.bpm);
	if (_resolved_hz >= 0.0 && resolved_hz != _resolved_hz) {
		_transition_pending = true;
	}
	_resolved_hz = resolved_hz;
	_lfo.set_hz(resolved_hz, p_context.sample_rate);

	if (_transition_pending && _left_gain.is_initialized()) {
		const int transition_length = MAX((int)(sion::dsp::LFO_EDGE_SECONDS * p_context.sample_rate), 2);
		_left_gain.begin(transition_length);
		_right_gain.begin(transition_length);
	}
	_transition_pending = false;

	double *buffer = r_buffer->ptrw();
	const int begin = p_start_index << 1;
	const int end = (p_start_index + p_length) << 1;
	sion::dsp::with_lfo_shape(_shape, [&](auto p_shape) {
		_process(p_shape, buffer, begin, end);
	});

	return 2;
}

void SiEffectAutopan::set_by_mml(Vector<double> p_args) {
	double frequency = _get_mml_arg(p_args, 0, 1);
	double stereo_width = _get_mml_arg(p_args, 1, 100) / 100.0;
	int rate_mode = _get_mml_arg(p_args, 2, (int)LfoRateMode::HZ);
	int sync_division = _get_mml_arg(p_args, 3, 2);
	int shape = _get_mml_arg(p_args, 4, (int)LfoShape::SINE);
	double time = _get_mml_arg(p_args, 5, 1000);

	set_params(frequency, stereo_width, rate_mode, sync_division, shape, time);
}

bool SiEffectAutopan::set_arg(int p_arg_index, double p_value) {
	double frequency = _rate.hz;
	double stereo_width = _stereo_width;
	int rate_mode = (int)_rate.mode;
	int sync_division = _rate.division;
	int shape = (int)_shape;
	double time = _rate.ms;

	switch (p_arg_index) {
		case 0:
			frequency = p_value;
			break;
		case 1:
			stereo_width = p_value / 100.0;
			break;
		case 2:
			rate_mode = (int)p_value;
			break;
		case 3:
			sync_division = (int)p_value;
			break;
		case 4:
			shape = (int)p_value;
			break;
		case 5:
			time = p_value;
			break;
		default:
			return false;
	}

	set_params(frequency, stereo_width, rate_mode, sync_division, shape, time);
	return true;
}

void SiEffectAutopan::reset() {
	// A quarter cycle in, the left channel starts at full gain.
	_lfo.reset(0.25);
	set_params();
	_resolved_hz = -1.0;
	_transition_pending = false;
	_left_gain.reset();
	_right_gain.reset();
}

void SiEffectAutopan::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_params", "frequency", "stereo_width", "rate_mode", "sync_division", "shape", "time"), &SiEffectAutopan::set_params, DEFVAL(1), DEFVAL(1), DEFVAL(0), DEFVAL(2), DEFVAL(0), DEFVAL(1000));
}

SiEffectAutopan::SiEffectAutopan(double p_frequency, double p_stereo_width) :
		SiEffectBase() {
	reset();
	set_params(p_frequency, p_stereo_width);
}
