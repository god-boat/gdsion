/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_shear_distort.h"

#include "dsp/fast_tanh.h"

using sion::dsp::fast_tanh;
using sion::dsp::HALFBAND_LIGHT;
using sion::dsp::HALFBAND_STEEP;
using sion::dsp::LfoShape;
using sion::dsp::one_pole_coeff;

const double SiEffectShearDistort::BODY_CROSSOVER_HZ = 200.0;
const double SiEffectShearDistort::TILT_PIVOT_HZ = 700.0;
const double SiEffectShearDistort::DC_BLOCK_HZ = 5.0;
const double SiEffectShearDistort::ENV_RELEASE_HZ = 2.0; // ~80 ms release
const double SiEffectShearDistort::ENV_FLOOR = 1e-6;
const double SiEffectShearDistort::SHEAR_LFO_RATE_1 = 0.03;
const double SiEffectShearDistort::SHEAR_LFO_RATE_2 = 0.04854101966249685; // rate1 * golden ratio

// --- ToneSVF ----------------------------------------------------------------------

double SiEffectShearDistort::ToneSVF::process_lowpass(double p_input, double p_g) {
	const double k = 1.4142135623730951; // 1/Q, Butterworth.
	double a1 = 1.0 / (1.0 + p_g * (p_g + k));
	double v1 = a1 * (ic1 + p_g * (p_input - ic2));
	double v2 = ic2 + p_g * v1;
	ic1 = 2.0 * v1 - ic1;
	ic2 = 2.0 * v2 - ic2;
	return v2;
}

void SiEffectShearDistort::ToneSVF::clear() {
	ic1 = 0.0;
	ic2 = 0.0;
}

void SiEffectShearDistort::ToneSVF::flush_denormals() {
	if (Math::abs(ic1) < 1e-15) {
		ic1 = 0.0;
	}
	if (Math::abs(ic2) < 1e-15) {
		ic2 = 0.0;
	}
}

// --- SmoothedParam ------------------------------------------------------------------

void SiEffectShearDistort::SmoothedParam::flush_denormals() {
	if (Math::abs(current - target) < 1e-12) {
		current = target;
	}
}

// --- ChannelState ---------------------------------------------------------------

void SiEffectShearDistort::ChannelState::clear() {
	body_lpf = {};
	tilt_pre_lpf = {};
	tilt_post_lpf = {};
	dc = {};
	env = 0.0;
	tone_svf.clear();
	up_steep = {};
	up_light = {};
	down_light = {};
	down_steep = {};
}

void SiEffectShearDistort::ChannelState::flush_denormals() {
	tone_svf.flush_denormals();
}

// --- Waveshaper -----------------------------------------------------------------
//
// Shape morphs the waveform before one saturator, in envelope-relative units:
// u = input / peak envelope, so |u| peaks near 1 at any input level. Drive
// only pushes the saturator. At low drive the output follows the stop's
// waveform; at full drive it becomes a square wave switching at the stop's
// zero crossings, which saturation cannot flatten.
//
// All curves are C1-continuous (no slope discontinuities), which keeps the
// harmonic series rolling off fast enough for 4x oversampling to handle.

double SiEffectShearDistort::_stop_tube(double p_u) {
	// The square term adds a 2nd harmonic directly and moves the zero crossing,
	// so full drive gives an asymmetric pulse. Centered on a full-envelope sine
	// (mean of u^2 is 0.5), so at low drive it adds no DC to thump at note onsets.
	return p_u + 0.4 * (p_u * p_u - 0.5);
}

double SiEffectShearDistort::_stop_fold(double p_u) {
	// Folds back through zero near each peak: six edges per cycle at full drive.
	return Math::sin(1.5 * M_PI * p_u);
}

double SiEffectShearDistort::_stop_octave(double p_u) {
	// Full-wave rectified around a rounded corner, so the pulses run at twice the
	// input frequency. Recentered on the rectified full-envelope sine's mean, so
	// at low drive it adds no DC to thump at note onsets.
	const double corner = 0.05;
	const double rectified_sine_mean = 0.5905;
	return 2.0 * (Math::sqrt(p_u * p_u + corner * corner) - corner - rectified_sine_mean);
}

double SiEffectShearDistort::_shape_sample(double p_u, double p_gain, double p_bias, double p_shape) {
	double u = p_u + p_bias;
	double pos = p_shape * 3.0;
	int region = MIN((int)pos, 2);
	double t = pos - (double)region;

	// Stops: warm (u itself) -> tube -> fold -> octave.
	double g_a, g_b;
	switch (region) {
		case 0:
			g_a = u;
			g_b = _stop_tube(u);
			break;
		case 1:
			g_a = _stop_tube(u);
			g_b = _stop_fold(u);
			break;
		default:
			g_a = _stop_fold(u);
			g_b = _stop_octave(u);
			break;
	}

	// Blending before the saturator makes in-between positions new waveforms,
	// not a mix of two finished outputs.
	return fast_tanh(p_gain * (g_a + (g_b - g_a) * t));
}

// --- Internal parameter updates -------------------------------------------------

void SiEffectShearDistort::_update_derived() {
	// Drive 0..1 -> 0..+48 dB with a perceptual curve (finer control at the
	// warm low end, where the ear is most sensitive to saturation amount).
	double drive_db = 48.0 * Math::pow(_p_drive, 1.5);
	double drive_linear = Math::pow(10.0, drive_db / 20.0);
	// Bias is a fraction of the signal peak, so at full drive it sets pulse
	// width instead of vanishing under the gain.
	double bias_term = _p_bias * 0.8;

	// Auto makeup: measure the plain saturator (the warm stop) at a fixed set of
	// peak levels and normalize toward 0.5, so Drive and Bias track dry level
	// without leveling Shape differences away.
	static const double refs[3] = { 0.35, 0.55, 0.75 };
	double sum = 0.0;
	for (int i = 0; i < 3; i++) {
		double pos = _shape_sample(1.0, drive_linear * refs[i], bias_term, 0.0);
		double neg = _shape_sample(-1.0, drive_linear * refs[i], bias_term, 0.0);
		double swing = (pos - neg) * 0.5;
		sum += swing * swing;
	}
	double rms = Math::sqrt(sum / 3.0);
	double makeup = CLAMP(0.5 / MAX(rms, 0.03), 0.25, 2.5);

	// Tone tilt: brighten into the shaper so highs get driven (rich harmonics),
	// then partially undo it afterwards so the result isn't fizzy.
	double tilt = _p_tone - 0.5;
	double tilt_pre = Math::pow(10.0, tilt * 14.0 / 20.0);
	double tilt_post = Math::pow(10.0, -tilt * 9.0 / 20.0);

	_sm_drive.set_target(drive_linear);
	_sm_bias.set_target(bias_term);
	_sm_makeup.set_target(makeup);
	_sm_side.set_target(_p_width * 2.0);
	_sm_body.set_target(_p_body);
	_sm_shape.set_target(_p_shape);
	_sm_tilt_pre.set_target(tilt_pre);
	_sm_tilt_post.set_target(tilt_post);
	_sm_mix.set_target(_p_mix);
}

void SiEffectShearDistort::_update_filters() {
	double sr = _get_sampling_rate();

	_body_coeff = one_pole_coeff(BODY_CROSSOVER_HZ, sr);
	_tilt_coeff = one_pole_coeff(TILT_PIVOT_HZ, sr);
	_dc_coeff = one_pole_coeff(DC_BLOCK_HZ, sr);
	_env_decay = 1.0 - one_pole_coeff(ENV_RELEASE_HZ, sr);

	// Tone 0..1 -> post lowpass 550 Hz .. fully open (exponential sweep).
	double tone_hz = MIN(550.0 * Math::pow(40.0, _p_tone), sr * 0.45);
	_sm_tone_g.set_target(Math::tan(M_PI * tone_hz / sr));

	// ~5 ms parameter smoothing.
	_smooth_coeff = 1.0 - ::exp(-1.0 / (0.005 * sr));
}

void SiEffectShearDistort::_snap_smoothers() {
	_sm_drive.snap();
	_sm_bias.snap();
	_sm_makeup.snap();
	_sm_side.snap();
	_sm_body.snap();
	_sm_shape.snap();
	_sm_tilt_pre.snap();
	_sm_tilt_post.snap();
	_sm_tone_g.snap();
	_sm_mix.snap();
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
	_update_derived();
	_update_filters();
	_snap_smoothers();
	_left.clear();
	_right.clear();
	return 2;
}

double SiEffectShearDistort::_process_channel(ChannelState &p_ch, double p_input, double p_drive, double p_bias,
		double p_makeup, double p_body, double p_shape,
		double p_tilt_pre, double p_tilt_post, double p_tone_g) {
	// Body crossover: the one-pole split is exactly complementary, so
	// low + high reconstructs the input with no coloration.
	double low = p_ch.body_lpf.lowpass(_body_coeff, p_input);
	double high = p_input - low;
	double into = high + low * (1.0 - p_body);
	double kept_low = low * p_body;

	// Pre-emphasis tilt into the shaper.
	double tilt_low = p_ch.tilt_pre_lpf.lowpass(_tilt_coeff, into);
	into = tilt_low + (into - tilt_low) * p_tilt_pre;

	// Peak envelope: instant attack, ENV_RELEASE_HZ release, floored so silence
	// divides cleanly. The shaper sees the input relative to it.
	p_ch.env = MAX(Math::abs(into), MAX(p_ch.env * _env_decay, ENV_FLOOR));
	double inv_env = 1.0 / p_ch.env;
	double gain = p_drive * p_ch.env;

	// 4x oversampled waveshaping.
	double u0, u1;
	p_ch.up_steep.upsample(HALFBAND_STEEP, into, u0, u1);
	double s0, s1, s2, s3;
	p_ch.up_light.upsample(HALFBAND_LIGHT, u0, s0, s1);
	p_ch.up_light.upsample(HALFBAND_LIGHT, u1, s2, s3);

	s0 = _shape_sample(s0 * inv_env, gain, p_bias, p_shape);
	s1 = _shape_sample(s1 * inv_env, gain, p_bias, p_shape);
	s2 = _shape_sample(s2 * inv_env, gain, p_bias, p_shape);
	s3 = _shape_sample(s3 * inv_env, gain, p_bias, p_shape);

	double d0 = p_ch.down_light.downsample(HALFBAND_LIGHT, s0, s1);
	double d1 = p_ch.down_light.downsample(HALFBAND_LIGHT, s2, s3);
	double dist = p_ch.down_steep.downsample(HALFBAND_STEEP, d0, d1);

	dist *= p_makeup;

	// Remove the offset introduced by bias and asymmetric curves.
	dist = p_ch.dc.highpass(_dc_coeff, dist);

	// De-emphasis tilt, then the tone lowpass (distorted band only, so the
	// protected lows stay full even at dark tone settings).
	double post_low = p_ch.tilt_post_lpf.lowpass(_tilt_coeff, dist);
	dist = post_low + (dist - post_low) * p_tilt_post;
	dist = p_ch.tone_svf.process_lowpass(dist, p_tone_g);

	// Protected lows pass near-unity with a touch of glue saturation.
	double low_out = fast_tanh(kept_low * 1.25) * 0.8;

	return dist + low_out;
}

int SiEffectShearDistort::process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) {
	int start_index = p_start_index << 1;
	int length = p_length << 1;

	if (length <= 0) {
		return p_channels;
	}

	// The shear LFOs tick once per block (rate ≈ 0.03 Hz, negligible intra-block change).
	_shear_lfo1.increment = SHEAR_LFO_RATE_1 * p_length / p_context.sample_rate;
	_shear_lfo2.increment = SHEAR_LFO_RATE_2 * p_length / p_context.sample_rate;
	double shear_lfo1 = _shear_lfo1.tick<LfoShape::SINE>();
	double shear_lfo2 = _shear_lfo2.tick<LfoShape::SINE>();
	double shear_mod = (shear_lfo1 + shear_lfo2 * 0.3) * _p_shear;

	double drive_skew_l = 1.0 + shear_mod * 0.12;
	double drive_skew_r = 1.0 - shear_mod * 0.12;
	double bias_skew = shear_mod * 0.18;

	bool stereo = (p_channels >= 2);
	double smooth = _smooth_coeff;

	for (int i = start_index; i < start_index + length; i += 2) {
		// Tick parameter smoothers once per frame (shared by both channels).
		double drive = _sm_drive.tick(smooth);
		double bias = _sm_bias.tick(smooth);
		double makeup = _sm_makeup.tick(smooth);
		double side_drive = _sm_side.tick(smooth);
		double body = _sm_body.tick(smooth);
		double shape = _sm_shape.tick(smooth);
		double tilt_pre = _sm_tilt_pre.tick(smooth);
		double tilt_post = _sm_tilt_post.tick(smooth);
		double tone_g = _sm_tone_g.tick(smooth);
		double mix = _sm_mix.tick(smooth);

		double dry_l = (*r_buffer)[i];
		double dry_r = stereo ? (*r_buffer)[i + 1] : dry_l;

		// Mid/side for width control.
		double mid = (dry_l + dry_r) * 0.5;
		double side = (dry_l - dry_r) * 0.5 * side_drive;

		double wet_l = _process_channel(_left, mid + side,
				drive * drive_skew_l, bias + bias_skew,
				makeup, body, shape, tilt_pre, tilt_post, tone_g);
		double wet_r = _process_channel(_right, mid - side,
				drive * drive_skew_r, bias - bias_skew,
				makeup, body, shape, tilt_pre, tilt_post, tone_g);

		r_buffer->write[i] = dry_l + (wet_l - dry_l) * mix;
		r_buffer->write[i + 1] = dry_r + (wet_r - dry_r) * mix;
	}

	// Flush denormals.
	_left.flush_denormals();
	_right.flush_denormals();
	_sm_drive.flush_denormals();
	_sm_bias.flush_denormals();
	_sm_makeup.flush_denormals();
	_sm_side.flush_denormals();
	_sm_body.flush_denormals();
	_sm_shape.flush_denormals();
	_sm_tilt_pre.flush_denormals();
	_sm_tilt_post.flush_denormals();
	_sm_tone_g.flush_denormals();
	_sm_mix.flush_denormals();

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
	double drive = _get_mml_arg(p_args, 0, 0.4);
	double shape = _get_mml_arg(p_args, 1, 0.25);
	double bias = _get_mml_arg(p_args, 2, 0.0);
	double tone = _get_mml_arg(p_args, 3, 0.6);
	double body = _get_mml_arg(p_args, 4, 0.5);
	double width = _get_mml_arg(p_args, 5, 0.5);
	double shear = _get_mml_arg(p_args, 6, 0.15);
	double mix = _get_mml_arg(p_args, 7, 1.0);

	set_params(drive, shape, bias, tone, body, width, shear, mix);
}

void SiEffectShearDistort::reset() {
	_p_drive = 0.4;
	_p_shape = 0.25;
	_p_bias = 0.0;
	_p_tone = 0.6;
	_p_body = 0.5;
	_p_width = 0.5;
	_p_shear = 0.15;
	_p_mix = 1.0;

	_shear_lfo1.reset();
	_shear_lfo2.reset();

	_left.clear();
	_right.clear();

	_update_derived();
	_update_filters();
	_snap_smoothers();
}

void SiEffectShearDistort::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("set_params", "drive", "shape", "bias", "tone", "body", "width", "shear", "mix"),
			&SiEffectShearDistort::set_params,
			DEFVAL(0.4), DEFVAL(0.25), DEFVAL(0.0), DEFVAL(0.6),
			DEFVAL(0.5), DEFVAL(0.5), DEFVAL(0.15), DEFVAL(1.0));
}

SiEffectShearDistort::SiEffectShearDistort() :
		SiEffectBase() {
	reset();
}
