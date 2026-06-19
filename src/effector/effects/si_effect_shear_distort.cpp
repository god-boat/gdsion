/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#include "si_effect_shear_distort.h"

const double SiEffectShearDistort::BODY_CROSSOVER_HZ = 200.0;
const double SiEffectShearDistort::TILT_PIVOT_HZ = 700.0;
const double SiEffectShearDistort::DC_BLOCK_HZ = 5.0;
const double SiEffectShearDistort::SHEAR_LFO_RATE_1 = 0.03;
const double SiEffectShearDistort::SHEAR_LFO_RATE_2 = 0.04854101966249685; // rate1 * golden ratio

// Two-path polyphase allpass halfband coefficients (classic public-domain
// sets). "Steep" rejects ~69 dB with a 0.01*fs transition band and protects
// the audible range at the 1x<->2x boundary. "Light" rejects ~70 dB with a
// relaxed 0.1*fs transition, plenty for the 2x<->4x octave where the signal
// only occupies the bottom quarter of the spectrum.
static const double HALFBAND_STEEP_A[4] = { 0.07711507983241622, 0.4820706250610472, 0.7968204713315797, 0.9412514277740471 };
static const double HALFBAND_STEEP_B[4] = { 0.2659685265210946, 0.6651041532634957, 0.8841015085506159, 0.9820054141886075 };
static const double HALFBAND_LIGHT_A[2] = { 0.07986642623635751, 0.5453536510711322 };
static const double HALFBAND_LIGHT_B[2] = { 0.28382934487410993, 0.8344118914807379 };

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

void SiEffectShearDistort::DcBlocker::set_cutoff(double p_hz, double p_sample_rate) {
	r = CLAMP(1.0 - 2.0 * M_PI * p_hz / p_sample_rate, 0.9, 0.99999);
}

double SiEffectShearDistort::DcBlocker::process(double p_input) {
	double y = p_input - x1 + r * y1;
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

// --- AllpassSection ---------------------------------------------------------------

double SiEffectShearDistort::AllpassSection::process(double p_input) {
	double y = x1 + c * (p_input - y1);
	x1 = p_input;
	y1 = y;
	return y;
}

void SiEffectShearDistort::AllpassSection::clear() {
	x1 = 0.0;
	y1 = 0.0;
}

void SiEffectShearDistort::AllpassSection::flush_denormals() {
	if (Math::abs(x1) < 1e-15) {
		x1 = 0.0;
	}
	if (Math::abs(y1) < 1e-15) {
		y1 = 0.0;
	}
}

// --- HalfbandStage ----------------------------------------------------------------

void SiEffectShearDistort::HalfbandStage::setup(const double *p_coeffs_a, const double *p_coeffs_b, int p_count) {
	section_count = MIN(p_count, MAX_SECTIONS);
	for (int i = 0; i < section_count; i++) {
		path_a[i].c = p_coeffs_a[i];
		path_b[i].c = p_coeffs_b[i];
	}
}

double SiEffectShearDistort::HalfbandStage::_run_path(AllpassSection *p_path, double p_input) {
	double value = p_input;
	for (int i = 0; i < section_count; i++) {
		value = p_path[i].process(value);
	}
	return value;
}

void SiEffectShearDistort::HalfbandStage::upsample(double p_input, double &r_out0, double &r_out1) {
	r_out0 = _run_path(path_a, p_input);
	r_out1 = _run_path(path_b, p_input);
}

double SiEffectShearDistort::HalfbandStage::downsample(double p_input0, double p_input1) {
	return 0.5 * (_run_path(path_a, p_input1) + _run_path(path_b, p_input0));
}

void SiEffectShearDistort::HalfbandStage::clear() {
	for (int i = 0; i < section_count; i++) {
		path_a[i].clear();
		path_b[i].clear();
	}
}

void SiEffectShearDistort::HalfbandStage::flush_denormals() {
	for (int i = 0; i < section_count; i++) {
		path_a[i].flush_denormals();
		path_b[i].flush_denormals();
	}
}

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

void SiEffectShearDistort::ChannelState::setup() {
	up_steep.setup(HALFBAND_STEEP_A, HALFBAND_STEEP_B, 4);
	down_steep.setup(HALFBAND_STEEP_A, HALFBAND_STEEP_B, 4);
	up_light.setup(HALFBAND_LIGHT_A, HALFBAND_LIGHT_B, 2);
	down_light.setup(HALFBAND_LIGHT_A, HALFBAND_LIGHT_B, 2);
}

void SiEffectShearDistort::ChannelState::clear() {
	body_lpf.clear();
	tilt_pre_lpf.clear();
	tilt_post_lpf.clear();
	dc.clear();
	tone_svf.clear();
	up_steep.clear();
	up_light.clear();
	down_light.clear();
	down_steep.clear();
}

void SiEffectShearDistort::ChannelState::flush_denormals() {
	body_lpf.flush_denormals();
	tilt_pre_lpf.flush_denormals();
	tilt_post_lpf.flush_denormals();
	dc.flush_denormals();
	tone_svf.flush_denormals();
	up_steep.flush_denormals();
	up_light.flush_denormals();
	down_light.flush_denormals();
	down_steep.flush_denormals();
}

// --- Distortion curves ----------------------------------------------------------
//
// All curves are C1-continuous (no slope discontinuities), which keeps the
// harmonic series rolling off fast enough for 4x oversampling to handle.

double SiEffectShearDistort::_fast_tanh(double p_x) {
	// Pade approximation of tanh; exact ±1 with zero slope at |x| = 3.
	if (p_x >= 3.0) {
		return 1.0;
	}
	if (p_x <= -3.0) {
		return -1.0;
	}
	double x2 = p_x * p_x;
	return p_x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

double SiEffectShearDistort::_curve_warm(double p_x) {
	return _fast_tanh(p_x);
}

double SiEffectShearDistort::_curve_tube(double p_x) {
	// tanh plus an even-symmetric term in tanh-space: adds even harmonics
	// (tube character) while staying smooth, bounded and zero at rest.
	double t = _fast_tanh(p_x * 1.1);
	double t2 = t * t;
	return t + 0.55 * t2 * (1.0 - t2);
}

double SiEffectShearDistort::_curve_clip(double p_x) {
	// Cubic soft clip: hard-clip aggression but C1 at the corners.
	if (p_x >= 1.5) {
		return 1.0;
	}
	if (p_x <= -1.5) {
		return -1.0;
	}
	return p_x - p_x * p_x * p_x * (4.0 / 27.0);
}

double SiEffectShearDistort::_curve_fold(double p_x) {
	// Sine fold: smooth everywhere, far fewer aliases than a triangle fold.
	return Math::sin(p_x * (M_PI * 0.5));
}

double SiEffectShearDistort::_shape_sample(double p_input, double p_shape) {
	double pos = p_shape * 3.0;
	int region = MIN((int)pos, 2);
	double t = pos - (double)region;

	double y_a, y_b;
	switch (region) {
		case 0:
			y_a = _curve_warm(p_input);
			y_b = _curve_tube(p_input);
			break;
		case 1:
			y_a = _curve_tube(p_input);
			y_b = _curve_clip(p_input);
			break;
		default:
			y_a = _curve_clip(p_input);
			y_b = _curve_fold(p_input);
			break;
	}

	return y_a + (y_b - y_a) * t;
}

// --- Internal parameter updates -------------------------------------------------

void SiEffectShearDistort::_update_derived() {
	// Drive 0..1 -> 0..+48 dB with a perceptual curve (finer control at the
	// warm low end, where the ear is most sensitive to saturation amount).
	double drive_db = 48.0 * Math::pow(_p_drive, 1.5);
	double drive_linear = Math::pow(10.0, drive_db / 20.0);
	double bias_term = _p_bias * 1.5;

	// Auto makeup: measure the shaper's AC swing at a few reference
	// amplitudes and normalize toward 0.5, so wet level tracks dry level
	// instead of growing with drive. This also keeps the distorted band
	// balanced against the protected low band.
	static const double refs[3] = { 0.35, 0.55, 0.75 };
	double sum = 0.0;
	for (int i = 0; i < 3; i++) {
		double pos = _shape_sample(refs[i] * drive_linear + bias_term, _p_shape);
		double neg = _shape_sample(-refs[i] * drive_linear + bias_term, _p_shape);
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

	_left.body_lpf.set_cutoff(BODY_CROSSOVER_HZ, sr);
	_right.body_lpf.set_cutoff(BODY_CROSSOVER_HZ, sr);

	_left.tilt_pre_lpf.set_cutoff(TILT_PIVOT_HZ, sr);
	_right.tilt_pre_lpf.set_cutoff(TILT_PIVOT_HZ, sr);
	_left.tilt_post_lpf.set_cutoff(TILT_PIVOT_HZ, sr);
	_right.tilt_post_lpf.set_cutoff(TILT_PIVOT_HZ, sr);

	_left.dc.set_cutoff(DC_BLOCK_HZ, sr);
	_right.dc.set_cutoff(DC_BLOCK_HZ, sr);

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
	double low = p_ch.body_lpf.process(p_input);
	double high = p_input - low;
	double into = high + low * (1.0 - p_body);
	double kept_low = low * p_body;

	// Pre-emphasis tilt into the shaper.
	double tilt_low = p_ch.tilt_pre_lpf.process(into);
	into = tilt_low + (into - tilt_low) * p_tilt_pre;

	// Drive and bias are linear/DC, so they commute with the upsampler and
	// can be applied at the base rate.
	double driven = into * p_drive + p_bias;

	// 4x oversampled waveshaping.
	double u0, u1;
	p_ch.up_steep.upsample(driven, u0, u1);
	double s0, s1, s2, s3;
	p_ch.up_light.upsample(u0, s0, s1);
	p_ch.up_light.upsample(u1, s2, s3);

	s0 = _shape_sample(s0, p_shape);
	s1 = _shape_sample(s1, p_shape);
	s2 = _shape_sample(s2, p_shape);
	s3 = _shape_sample(s3, p_shape);

	double d0 = p_ch.down_light.downsample(s0, s1);
	double d1 = p_ch.down_light.downsample(s2, s3);
	double dist = p_ch.down_steep.downsample(d0, d1);

	dist *= p_makeup;

	// Remove the offset introduced by bias and asymmetric curves.
	dist = p_ch.dc.process(dist);

	// De-emphasis tilt, then the tone lowpass (distorted band only, so the
	// protected lows stay full even at dark tone settings).
	double post_low = p_ch.tilt_post_lpf.process(dist);
	dist = post_low + (dist - post_low) * p_tilt_post;
	dist = p_ch.tone_svf.process_lowpass(dist, p_tone_g);

	// Protected lows pass near-unity with a touch of glue saturation.
	double low_out = _fast_tanh(kept_low * 1.25) * 0.8;

	return dist + low_out;
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

	_shear_phase = 0.0;
	_shear_phase2 = 0.0;

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
	_left.setup();
	_right.setup();
	reset();
}
