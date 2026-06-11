/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_shear_distort.h"

const double SiEffectShearDistort::BODY_CROSSOVER_HZ = 250.0;
const double SiEffectShearDistort::PRE_EMPH_CENTER_HZ = 1500.0;
const double SiEffectShearDistort::SHEAR_LFO_RATE_1 = 0.03;
const double SiEffectShearDistort::SHEAR_LFO_RATE_2 = 0.04854101966249685; // rate1 * golden ratio

// --- OnePoleLPF -----------------------------------------------------------------

void SiEffectShearDistort::OnePoleLPF::set_cutoff(double p_hz, double p_sample_rate) {
	coeff = ::exp(-2.0 * M_PI * p_hz / p_sample_rate);
}

double SiEffectShearDistort::OnePoleLPF::process(double p_input) {
	z = p_input + (z - p_input) * coeff;
	return z;
}

void SiEffectShearDistort::OnePoleLPF::clear() {
	z = 0.0;
}

void SiEffectShearDistort::OnePoleLPF::flush_denormals() {
	if (Math::abs(z) < 1e-15) {
		z = 0.0;
	}
}

// --- DcBlocker ------------------------------------------------------------------

double SiEffectShearDistort::DcBlocker::process(double p_input) {
	double y = p_input - x1 + 0.995 * y1;
	x1 = p_input;
	y1 = y;
	return y;
}

void SiEffectShearDistort::DcBlocker::clear() {
	x1 = 0.0;
	y1 = 0.0;
}

void SiEffectShearDistort::DcBlocker::flush_denormals() {
	if (Math::abs(x1) < 1e-15) {
		x1 = 0.0;
	}
	if (Math::abs(y1) < 1e-15) {
		y1 = 0.0;
	}
}

// --- ChannelState ---------------------------------------------------------------

void SiEffectShearDistort::ChannelState::clear() {
	body_lpf.clear();
	pre_emph_lpf.clear();
	tone_lpf.clear();
	dc.clear();
}

void SiEffectShearDistort::ChannelState::flush_denormals() {
	body_lpf.flush_denormals();
	pre_emph_lpf.flush_denormals();
	tone_lpf.flush_denormals();
	dc.flush_denormals();
}

// --- Distortion curves ----------------------------------------------------------

double SiEffectShearDistort::_soft_tanh(double p_x) {
	return Math::tanh(p_x);
}

double SiEffectShearDistort::_asymmetric(double p_x) {
	if (p_x >= 0.0) {
		return Math::tanh(p_x * 1.25);
	}
	return Math::tanh(p_x * 0.75);
}

double SiEffectShearDistort::_hard_clip(double p_x) {
	return CLAMP(p_x, -1.0, 1.0);
}

double SiEffectShearDistort::_fold(double p_x) {
	p_x = ::fmod(p_x + 1.0, 4.0);
	if (p_x < 0.0) {
		p_x += 4.0;
	}
	return p_x < 2.0 ? p_x - 1.0 : 3.0 - p_x;
}

double SiEffectShearDistort::_shape_sample(double p_input, double p_shape) {
	double pos = p_shape * 3.0;
	int region = MIN((int)pos, 2);
	double t = pos - (double)region;

	double y_a, y_b;
	switch (region) {
		case 0:
			y_a = _soft_tanh(p_input);
			y_b = _asymmetric(p_input);
			break;
		case 1:
			y_a = _asymmetric(p_input);
			y_b = _hard_clip(p_input);
			break;
		default:
			y_a = _hard_clip(p_input);
			y_b = _fold(p_input);
			break;
	}

	return y_a + (y_b - y_a) * t;
}

// --- Internal parameter updates -------------------------------------------------

void SiEffectShearDistort::_update_derived() {
	// Drive 0..1 → linear gain 1..~251 (0 to 48 dB).
	_drive_linear = Math::pow(10.0, _p_drive * 48.0 / 20.0);

	// Width 0..1 → side multiplier 0..2 (0=mono, 0.5=unity, 1=enhanced).
	_side_drive = _p_width * 2.0;

	// Tone 0..1 → pre-emphasis amount -1..+1 (darken / brighten before shaper).
	_pre_emph_amount = (_p_tone - 0.5) * 2.0;
}

void SiEffectShearDistort::_update_filters() {
	double sr = _get_sampling_rate();

	_left.body_lpf.set_cutoff(BODY_CROSSOVER_HZ, sr);
	_right.body_lpf.set_cutoff(BODY_CROSSOVER_HZ, sr);

	_left.pre_emph_lpf.set_cutoff(PRE_EMPH_CENTER_HZ, sr);
	_right.pre_emph_lpf.set_cutoff(PRE_EMPH_CENTER_HZ, sr);

	// Tone 0..1 → post-distortion LPF 800..16000 Hz (exponential).
	double tone_hz = 800.0 * Math::pow(20.0, _p_tone);
	_left.tone_lpf.set_cutoff(tone_hz, sr);
	_right.tone_lpf.set_cutoff(tone_hz, sr);
}

// --- Public API -----------------------------------------------------------------

void SiEffectShearDistort::set_params(double p_drive, double p_shape, double p_bias,
		double p_tone, double p_body, double p_width,
		double p_shear, double p_mix) {
	_p_drive = CLAMP(p_drive, 0.0, 1.0);
	_p_shape = CLAMP(p_shape, 0.0, 1.0);
	_p_bias = CLAMP(p_bias, -1.0, 1.0);
	_p_tone = CLAMP(p_tone, 0.0, 1.0);
	_p_body = CLAMP(p_body, 0.0, 1.0);
	_p_width = CLAMP(p_width, 0.0, 1.0);
	_p_shear = CLAMP(p_shear, 0.0, 1.0);
	_p_mix = CLAMP(p_mix, 0.0, 1.0);

	_update_derived();
	_update_filters();
}

// --- SiEffectBase overrides -----------------------------------------------------

int SiEffectShearDistort::prepare_process() {
	_left.clear();
	_right.clear();
	return 2;
}

int SiEffectShearDistort::process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	int start_index = p_start_index << 1;
	int length = p_length << 1;

	if (length <= 0) {
		return p_channels;
	}

	// Compute shear LFO once per block (rate ≈ 0.03 Hz, negligible intra-block change).
	double shear_lfo1 = Math::sin(_shear_phase * Math_TAU);
	double shear_lfo2 = Math::sin(_shear_phase2 * Math_TAU);
	double shear_mod = (shear_lfo1 + shear_lfo2 * 0.3) * _p_shear;

	double drive_l_mod = _drive_linear * (1.0 + shear_mod * 0.15);
	double drive_r_mod = _drive_linear * (1.0 - shear_mod * 0.15);
	double bias_l_mod = _p_bias + shear_mod * 0.2;
	double bias_r_mod = _p_bias - shear_mod * 0.2;

	double mix_dry = 1.0 - _p_mix;
	double mix_wet = _p_mix;

	bool stereo = (p_channels >= 2);

	for (int i = start_index; i < start_index + length; i += 2) {
		double dry_l = (*r_buffer)[i];
		double dry_r = stereo ? (*r_buffer)[i + 1] : dry_l;

		// Mid/side for width control.
		double mid = (dry_l + dry_r) * 0.5;
		double side = (dry_l - dry_r) * 0.5;
		side *= _side_drive;

		double proc_l = mid + side;
		double proc_r = mid - side;

		// Body split: separate lows from mids/highs.
		double low_l = _left.body_lpf.process(proc_l);
		double high_l = proc_l - low_l;
		double low_r = _right.body_lpf.process(proc_r);
		double high_r = proc_r - low_r;

		// Pre-emphasis: extract high content and blend based on tone.
		double pre_low_l = _left.pre_emph_lpf.process(high_l);
		high_l += (high_l - pre_low_l) * _pre_emph_amount * 0.5;

		double pre_low_r = _right.pre_emph_lpf.process(high_r);
		high_r += (high_r - pre_low_r) * _pre_emph_amount * 0.5;

		// Drive + bias + shape morph (mids/highs).
		double dist_l = _shape_sample(high_l * drive_l_mod + bias_l_mod, _p_shape);
		double dist_r = _shape_sample(high_r * drive_r_mod + bias_r_mod, _p_shape);

		// DC block (removes offset introduced by bias).
		dist_l = _left.dc.process(dist_l);
		dist_r = _right.dc.process(dist_r);

		// Post-distortion tone filter.
		dist_l = _left.tone_lpf.process(dist_l);
		dist_r = _right.tone_lpf.process(dist_r);

		// Body recombine.
		// body=0 -> lows are also saturated, body=1 -> lows bypass the main distortion cleanly.
		double low_out_l = low_l * _p_body + _soft_tanh(low_l * drive_l_mod * 0.5) * (1.0 - _p_body);
		double low_out_r = low_r * _p_body + _soft_tanh(low_r * drive_r_mod * 0.5) * (1.0 - _p_body);

		double wet_l = dist_l + low_out_l;
		double wet_r = dist_r + low_out_r;

		// Dry/wet mix.
		r_buffer->write[i] = dry_l * mix_dry + wet_l * mix_wet;
		r_buffer->write[i + 1] = dry_r * mix_dry + wet_r * mix_wet;
	}

	// Advance shear LFO phases.
	double inv_sr = 1.0 / _get_sampling_rate();
	_shear_phase += SHEAR_LFO_RATE_1 * inv_sr * (double)p_length;
	while (_shear_phase >= 1.0) {
		_shear_phase -= 1.0;
	}
	_shear_phase2 += SHEAR_LFO_RATE_2 * inv_sr * (double)p_length;
	while (_shear_phase2 >= 1.0) {
		_shear_phase2 -= 1.0;
	}

	// Flush denormals.
	_left.flush_denormals();
	_right.flush_denormals();

	return 2;
}

bool SiEffectShearDistort::set_arg(int p_arg_index, double p_value) {
	switch (p_arg_index) {
		case 0: _p_drive = CLAMP(p_value, 0.0, 1.0); break;
		case 1: _p_shape = CLAMP(p_value, 0.0, 1.0); break;
		case 2: _p_bias = CLAMP(p_value, -1.0, 1.0); break;
		case 3: _p_tone = CLAMP(p_value, 0.0, 1.0); break;
		case 4: _p_body = CLAMP(p_value, 0.0, 1.0); break;
		case 5: _p_width = CLAMP(p_value, 0.0, 1.0); break;
		case 6: _p_shear = CLAMP(p_value, 0.0, 1.0); break;
		case 7: _p_mix = CLAMP(p_value, 0.0, 1.0); break;
		default: return false;
	}

	_update_derived();
	_update_filters();
	return true;
}

void SiEffectShearDistort::set_by_mml(Vector<double> p_args) {
	double drive = _get_mml_arg(p_args, 0, 0.35);
	double shape = _get_mml_arg(p_args, 1, 0.35);
	double bias = _get_mml_arg(p_args, 2, 0.0);
	double tone = _get_mml_arg(p_args, 3, 0.55);
	double body = _get_mml_arg(p_args, 4, 0.45);
	double width = _get_mml_arg(p_args, 5, 0.5);
	double shear = _get_mml_arg(p_args, 6, 0.1);
	double mix = _get_mml_arg(p_args, 7, 1.0);

	set_params(drive, shape, bias, tone, body, width, shear, mix);
}

void SiEffectShearDistort::reset() {
	_p_drive = 0.35;
	_p_shape = 0.35;
	_p_bias = 0.0;
	_p_tone = 0.55;
	_p_body = 0.45;
	_p_width = 0.5;
	_p_shear = 0.1;
	_p_mix = 1.0;

	_shear_phase = 0.0;
	_shear_phase2 = 0.0;

	_left.clear();
	_right.clear();

	_update_derived();
	_update_filters();
}

void SiEffectShearDistort::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("set_params", "drive", "shape", "bias", "tone", "body", "width", "shear", "mix"),
			&SiEffectShearDistort::set_params,
			DEFVAL(0.35), DEFVAL(0.35), DEFVAL(0.0), DEFVAL(0.55),
			DEFVAL(0.45), DEFVAL(0.5), DEFVAL(0.1), DEFVAL(1.0));
}

SiEffectShearDistort::SiEffectShearDistort() :
		SiEffectBase() {
	reset();
}
